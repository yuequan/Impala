// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains implementations for the JNI FeSupport interface.

#include "service/fe-support.h"

#include <boost/scoped_ptr.hpp>

#include "codegen/llvm-codegen.h"
#include "common/init.h"
#include "common/logging.h"
#include "common/status.h"
#include "exec/catalog-op-executor.h"
#include "exprs/expr.h"
#include "runtime/exec-env.h"
#include "runtime/runtime-state.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/lib-cache.h"
#include "runtime/client-cache.h"
#include "util/cpu-info.h"
#include "util/disk-info.h"
#include "util/dynamic-util.h"
#include "util/jni-util.h"
#include "util/mem-info.h"
#include "util/symbols-util.h"
#include "rpc/thrift-util.h"
#include "rpc/thrift-server.h"
#include "util/debug-util.h"
#include "gen-cpp/Data_types.h"
#include "gen-cpp/Frontend_types.h"

using namespace impala;
using namespace std;
using namespace boost;
using namespace apache::thrift::server;

// Called from the FE when it explicitly loads libfesupport.so for tests.
// This creates the minimal state necessary to service the other JNI calls.
// This is not called when we first start up the BE.
extern "C"
JNIEXPORT void JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeFeTestInit(
    JNIEnv* env, jclass caller_class) {
  DCHECK(ExecEnv::GetInstance() == NULL) << "This should only be called once from the FE";
  char* name = const_cast<char*>("FeSupport");
  InitCommonRuntime(1, &name, false, TestInfo::FE_TEST);
  LlvmCodeGen::InitializeLlvm(true);
  ExecEnv* exec_env = new ExecEnv(); // This also caches it from the process.
  exec_env->InitForFeTests();
}

// Evaluates a batch of const exprs and returns the results in a serialized
// TResultRow, where each TColumnValue in the TResultRow stores the result of
// a predicate evaluation. It requires JniUtil::Init() to have been
// called.
extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeEvalConstExprs(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_expr_batch,
    jbyteArray thrift_query_ctx_bytes) {
  jbyteArray result_bytes = NULL;
  TQueryCtx query_ctx;
  TExprBatch expr_batch;
  JniLocalFrame jni_frame;
  TResultRow expr_results;
  ObjectPool obj_pool;

  DeserializeThriftMsg(env, thrift_expr_batch, &expr_batch);
  DeserializeThriftMsg(env, thrift_query_ctx_bytes, &query_ctx);
  RuntimeState state(query_ctx);

  THROW_IF_ERROR_RET(jni_frame.push(env), env, JniUtil::internal_exc_class(),
                     result_bytes);
  // Exprs can allocate memory so we need to set up the mem trackers before
  // preparing/running the exprs.
  THROW_IF_ERROR_RET(state.InitMemTrackers(TUniqueId(), NULL, -1), env,
                     JniUtil::internal_exc_class(), result_bytes);

  vector<TExpr>& exprs = expr_batch.exprs;
  // Prepate the exprs
  vector<Expr*> prepared_exprs;
  for (vector<TExpr>::iterator it = exprs.begin(); it != exprs.end(); it++) {
    Expr* e;
    THROW_IF_ERROR_RET(Expr::CreateExprTree(&obj_pool, *it, &e), env,
                       JniUtil::internal_exc_class(), result_bytes);
    THROW_IF_ERROR_RET(Expr::Prepare(e, &state, RowDescriptor()), env,
                       JniUtil::internal_exc_class(), result_bytes);
    prepared_exprs.push_back(e);
  }

  // Optimize the module so any UDF functions are jit'd
  if (state.codegen() != NULL) state.codegen()->FinalizeModule();

  vector<TColumnValue> results;
  // Evaluate the exprs
  for (vector<Expr*>::iterator it = prepared_exprs.begin();
       it != prepared_exprs.end(); it++) {
    TColumnValue val;
    THROW_IF_ERROR_RET((*it)->Open(&state), env,
                       JniUtil::internal_exc_class(), result_bytes);
    (*it)->GetValue(NULL, false, &val);
    (*it)->Close(&state);
    results.push_back(val);
  }
  expr_results.__set_colVals(results);
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &expr_results, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}

// Does the symbol resolution, filling in the result in *result.
static void ResolveSymbolLookup(const TSymbolLookupParams params,
    const vector<ColumnType>& arg_types, TSymbolLookupResult* result) {
  LibCache::LibType type;
  if (params.fn_binary_type == TFunctionBinaryType::NATIVE ||
      params.fn_binary_type == TFunctionBinaryType::BUILTIN) {
    // We use TYPE_SO for builtins, since LibCache does not resolve symbols for IR
    // builtins. This is ok since builtins have the same symbol whether we run the IR or
    // native versions.
    type = LibCache::TYPE_SO;
  } else if (params.fn_binary_type == TFunctionBinaryType::IR) {
    type = LibCache::TYPE_IR;
  } else if (params.fn_binary_type == TFunctionBinaryType::HIVE) {
    type = LibCache::TYPE_JAR;
  } else {
    DCHECK(false) << params.fn_binary_type;
  }

  // Builtin functions are loaded directly from the running process
  if (params.fn_binary_type != TFunctionBinaryType::BUILTIN) {
    // Refresh the library if necessary since we're creating a new function
    LibCache::instance()->SetNeedsRefresh(params.location);
    string dummy_local_path;
    Status status = LibCache::instance()->GetLocalLibPath(
        params.location, type, &dummy_local_path);
    if (!status.ok()) {
      result->__set_result_code(TSymbolLookupResultCode::BINARY_NOT_FOUND);
      result->__set_error_msg(status.GetErrorMsg());
      return;
    }
  }

  Status status =
      LibCache::instance()->CheckSymbolExists(params.location, type, params.symbol);
  if (status.ok()) {
    // The FE specified symbol exists, just use that.
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_FOUND);
    result->__set_symbol(params.symbol);
    // TODO: we can demangle the user symbol here and validate it against
    // params.arg_types. This would prevent someone from typing the wrong symbol
    // by accident. This requires more string parsing of the symbol.
    return;
  }

  // The input was already mangled and we couldn't find it, return the error.
  if (SymbolsUtil::IsMangled(params.symbol)) {
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_NOT_FOUND);
    stringstream ss;
    ss << "Could not find symbol '" << params.symbol << "' in: " << params.location;
    result->__set_error_msg(ss.str());
    return;
  }

  // Mangle the user input and do another lookup.
  string symbol;
  ColumnType ret_type(INVALID_TYPE);
  if (params.__isset.ret_arg_type) ret_type = ColumnType(params.ret_arg_type);

  if (params.symbol_type == TSymbolType::UDF_EVALUATE) {
    symbol = SymbolsUtil::MangleUserFunction(params.symbol,
        arg_types, params.has_var_args, params.__isset.ret_arg_type ? &ret_type : NULL);
  } else {
    DCHECK(params.symbol_type == TSymbolType::UDF_PREPARE ||
           params.symbol_type == TSymbolType::UDF_CLOSE);
    symbol = SymbolsUtil::ManglePrepareOrCloseFunction(params.symbol);
  }

  status = LibCache::instance()->CheckSymbolExists(params.location, type, symbol);
  if (!status.ok()) {
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_NOT_FOUND);
    stringstream ss;
    ss << "Could not find function " << params.symbol << "(";

    if (params.symbol_type == TSymbolType::UDF_EVALUATE) {
      for (int i = 0; i < arg_types.size(); ++i) {
        ss << arg_types[i].DebugString();
        if (i != arg_types.size() - 1) ss << ", ";
      }
    } else {
      ss << "impala_udf::FunctionContext*, "
         << "impala_udf::FunctionContext::FunctionStateScope";
    }

    ss << ")";
    if (params.__isset.ret_arg_type) ss << " returns " << ret_type.DebugString();
    ss << " in: " << params.location;
    if (params.__isset.ret_arg_type) {
      ss << "\nCheck that function name, arguments, and return type are correct.";
    } else {
      ss << "\nCheck that symbol and argument types are correct.";
    }
    result->__set_error_msg(ss.str());
    return;
  }

  // We were able to resolve the symbol.
  result->__set_result_code(TSymbolLookupResultCode::SYMBOL_FOUND);
  result->__set_symbol(symbol);
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeCacheJar(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_struct) {
  TCacheJarParams params;
  DeserializeThriftMsg(env, thrift_struct, &params);

  TCacheJarResult result;
  string local_path;
  Status status = LibCache::instance()->GetLocalLibPath(params.hdfs_location,
      LibCache::TYPE_JAR, &local_path);
  status.ToThrift(&result.status);
  if (status.ok()) result.__set_local_path(local_path);

  jbyteArray result_bytes = NULL;
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &result, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeLookupSymbol(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_struct) {
  TSymbolLookupParams lookup;
  DeserializeThriftMsg(env, thrift_struct, &lookup);

  vector<ColumnType> arg_types;
  for (int i = 0; i < lookup.arg_types.size(); ++i) {
    arg_types.push_back(ColumnType(lookup.arg_types[i]));
  }

  TSymbolLookupResult result;
  ResolveSymbolLookup(lookup, arg_types, &result);

  jbyteArray result_bytes = NULL;
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &result, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}

// Calls in to the catalog server to request prioritizing the loading of metadata for
// specific catalog objects.
extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativePrioritizeLoad(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_struct) {
  TPrioritizeLoadRequest request;
  DeserializeThriftMsg(env, thrift_struct, &request);

  CatalogOpExecutor catalog_op_executor(ExecEnv::GetInstance(), NULL);
  TPrioritizeLoadResponse result;
  Status status = catalog_op_executor.PrioritizeLoad(request, &result);
  if (!status.ok()) {
    LOG(ERROR) << status.GetErrorMsg();
    // Create a new Status, copy in this error, then update the result.
    Status catalog_service_status(result.status);
    catalog_service_status.AddError(status);
    status.ToThrift(&result.status);
  }

  jbyteArray result_bytes = NULL;
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &result, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}


namespace impala {

static JNINativeMethod native_methods[] = {
  {
    (char*)"NativeFeTestInit", (char*)"()V",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeFeTestInit
  },
  {
    (char*)"NativeEvalConstExprs", (char*)"([B[B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeEvalConstExprs
  },
  {
    (char*)"NativeCacheJar", (char*)"([B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeCacheJar
  },
  {
    (char*)"NativeLookupSymbol", (char*)"([B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeLookupSymbol
  },
  {
    (char*)"NativePrioritizeLoad", (char*)"([B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativePrioritizeLoad
  },
};

void InitFeSupport() {
  JNIEnv* env = getJNIEnv();
  jclass native_backend_cl = env->FindClass("com/cloudera/impala/service/FeSupport");
  env->RegisterNatives(native_backend_cl, native_methods,
      sizeof(native_methods) / sizeof(native_methods[0]));
  EXIT_IF_EXC(env);
}

}
