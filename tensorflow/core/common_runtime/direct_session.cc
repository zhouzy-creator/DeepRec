/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/common_runtime/direct_session.h"

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "absl/container/flat_hash_set.h"
#include "tensorflow/core/common_runtime/collective_executor_mgr.h"
#include "tensorflow/core/common_runtime/collective_param_resolver_local.h"
#include "tensorflow/core/common_runtime/constant_folding.h"
#include "tensorflow/core/common_runtime/debugger_state_interface.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_resolver_local.h"
#include "tensorflow/core/common_runtime/executor.h"
#include "tensorflow/core/common_runtime/executor_factory.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/graph_optimizer.h"
#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/common_runtime/memory_types.h"
#include "tensorflow/core/common_runtime/memory_planner.h"
#include "tensorflow/core/common_runtime/gpu_memory_planner.h"
#include "tensorflow/core/common_runtime/metrics.h"
#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/common_runtime/process_util.h"
#include "tensorflow/core/common_runtime/rendezvous_mgr.h"
#include "tensorflow/core/common_runtime/direct_session_group.h"
#include "tensorflow/core/common_runtime/scoped_allocator_mgr.h"
#include "tensorflow/core/common_runtime/step_stats_collector.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph.pb_text.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/graph_def_util.h"
#include "tensorflow/core/framework/log_memory.h"
#include "tensorflow/core/framework/logging.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/run_handler.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/graph/graph_partition.h"
#include "tensorflow/core/graph/subgraph.h"
#include "tensorflow/core/graph/tensor_id.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/core/threadpool_options.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/lib/gtl/stl_util.h"
#include "tensorflow/core/lib/monitoring/counter.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/byte_order.h"
#include "tensorflow/core/platform/cpu_info.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/tracing.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/lib/profiler_session.h"
#include "tensorflow/core/profiler/lib/traceme.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/util/device_name_utils.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {

namespace {

auto* direct_session_runs = monitoring::Counter<0>::New(
    "/tensorflow/core/direct_session_runs",
    "The number of times DirectSession::Run() has been called.");

Status NewThreadPoolFromThreadPoolOptions(
    const SessionOptions& options,
    const ThreadPoolOptionProto& thread_pool_options, int pool_number,
    thread::ThreadPool** pool, bool* owned) {
  int32 num_threads = thread_pool_options.num_threads();
  if (num_threads == 0) {
    num_threads = NumInterOpThreadsFromSessionOptions(options);
  }
  const string& name = thread_pool_options.global_name();
  if (name.empty()) {
    // Session-local threadpool.
    VLOG(1) << "Direct session inter op parallelism threads for pool "
            << pool_number << ": " << num_threads;
    *pool = new thread::ThreadPool(
        options.env, ThreadOptions(), strings::StrCat("Compute", pool_number),
        num_threads, !options.config.experimental().disable_thread_spinning(),
        /*allocator=*/nullptr);
    *owned = true;
    return Status::OK();
  }

  // Global, named threadpool.
  typedef std::pair<int32, thread::ThreadPool*> MapValue;
  static std::map<string, MapValue>* global_pool_map =
      new std::map<string, MapValue>;
  static mutex* mu = new mutex();
  mutex_lock l(*mu);
  MapValue* mvalue = &(*global_pool_map)[name];
  if (mvalue->second == nullptr) {
    mvalue->first = thread_pool_options.num_threads();
    mvalue->second = new thread::ThreadPool(
        options.env, ThreadOptions(), strings::StrCat("Compute", pool_number),
        num_threads, !options.config.experimental().disable_thread_spinning(),
        /*allocator=*/nullptr);
  } else {
    if (mvalue->first != thread_pool_options.num_threads()) {
      return errors::InvalidArgument(
          "Pool ", name,
          " configured previously with num_threads=", mvalue->first,
          "; cannot re-configure with num_threads=",
          thread_pool_options.num_threads());
    }
  }
  *owned = false;
  *pool = mvalue->second;
  return Status::OK();
}

thread::ThreadPool* GlobalThreadPool(const SessionOptions& options) {
  static thread::ThreadPool* const thread_pool =
      NewThreadPoolFromSessionOptions(options);
  return thread_pool;
}

// TODO(vrv): Figure out how to unify the many different functions
// that generate RendezvousKey, since many of them have to be
// consistent with each other.
string GetRendezvousKey(const string& tensor_name,
                        const DeviceAttributes& device_info,
                        const FrameAndIter& frame_iter) {
  return strings::StrCat(device_info.name(), ";",
                         strings::FpToString(device_info.incarnation()), ";",
                         device_info.name(), ";", tensor_name, ";",
                         frame_iter.frame_id, ":", frame_iter.iter_id);
}

// TODO: Any better allocate policy?
void AllocateVisibleCpusForSession(const std::vector<unsigned>& visible_cpus, int session_num,
                                   std::vector<std::vector<unsigned> >& visible_cpus_per_session) {
  if (session_num > 0) {
    int cpus_count_per_session = visible_cpus.size() / session_num;
    for (int i = 0; i < session_num; ++i) {
      std::vector<unsigned> tmp;
      int start_idx = i*cpus_count_per_session;
      tmp.insert(tmp.end(), visible_cpus.begin()+start_idx,
                 visible_cpus.begin()+start_idx+cpus_count_per_session);
      visible_cpus_per_session.push_back(tmp);
    }
  } else {
    LOG(FATAL) << "Session num of session group is " << session_num << ", should session_num > 0";
  }
}

}  // namespace

class DirectSessionFactory : public SessionFactory {
 public:
  DirectSessionFactory() {}

  bool AcceptsOptions(const SessionOptions& options) override {
    return options.target.empty();
  }

  Status NewSession(const SessionOptions& options,
                    Session** out_session) override {
    const auto& experimental_config = options.config.experimental();
    if (experimental_config.has_session_metadata()) {
      if (experimental_config.session_metadata().version() < 0) {
        return errors::InvalidArgument(
            "Session version shouldn't be negative: ",
            experimental_config.session_metadata().DebugString());
      }
      const string key = GetMetadataKey(experimental_config.session_metadata());
      mutex_lock l(sessions_lock_);
      if (!session_metadata_keys_.insert(key).second) {
        return errors::InvalidArgument(
            "A session with the same name and version has already been "
            "created: ",
            experimental_config.session_metadata().DebugString());
      }
    }

    // Must do this before the CPU allocator is created.
    if (options.config.graph_options().build_cost_model() > 0) {
      EnableCPUAllocatorFullStats(true);
    }
    std::vector<std::unique_ptr<Device>> devices;
    TF_RETURN_IF_ERROR(DeviceFactory::AddDevices(
        options, "/job:localhost/replica:0/task:0", &devices));

#ifdef TENSORFLOW_USE_NUMA
    std::vector<unsigned> visible_cpus;
    DirectSession* session =
        new DirectSession(options, new DeviceMgr(std::move(devices)),
                          true, this, visible_cpus);
#else
    DirectSession* session =
        new DirectSession(options, new DeviceMgr(std::move(devices)), true, this);
#endif
    {
      mutex_lock l(sessions_lock_);
      sessions_.push_back(session);
    }
    *out_session = session;
    return Status::OK();
  }

  Status NewSessionGroup(const SessionOptions& options,
                         SessionGroup** out_session_group,
                         int session_num = 1) {
    if (session_num < 1) {
      return errors::InvalidArgument(
          "Must specify session_num of NewSessionGroup");
    }

    const auto& experimental_config = options.config.experimental();
    if (experimental_config.has_session_metadata()) {
      if (experimental_config.session_metadata().version() < 0) {
        return errors::InvalidArgument(
            "Session version shouldn't be negative: ",
            experimental_config.session_metadata().DebugString());
      }
      const string key = GetMetadataKey(experimental_config.session_metadata());
      mutex_lock l(sessions_lock_);
      if (!session_metadata_keys_.insert(key).second) {
        return errors::InvalidArgument(
            "A session with the same name and version has already been "
            "created: ",
            experimental_config.session_metadata().DebugString());
      }
    }

    // Must do this before the CPU allocator is created.
    if (options.config.graph_options().build_cost_model() > 0) {
      EnableCPUAllocatorFullStats(true);
    }

#if GOOGLE_CUDA
    // Each virtual gpu device will be assigned to one session,
    // and every virtual device has a independent stream.
    bool use_multi_stream = options.config.use_per_session_stream();
    if (use_multi_stream) {
      int multi_streams_num = session_num;
      ConfigProto* config = const_cast<ConfigProto*>(&options.config);
      GPUOptions* gpu_options = config->mutable_gpu_options();
      auto virtual_devices =
          gpu_options->mutable_experimental()->add_virtual_devices();
      // will allocate gpu memory for each virtual device later.
      int32 mem_per_virtual_device = -1;
      for (int i = 0; i < multi_streams_num; ++i) {
        virtual_devices->add_memory_limit_mb(-1);
      }

      // We set allow_growth in multi-stream mode.
      gpu_options->set_allow_growth(true);
    } else {
      // NOTE: Use single stream in session group mode.
      // This can't get good performance.
      LOG(WARNING) << "Use single stream in session group mode,"
                   << "this can't get good performance.";
    }
#endif // GOOGLE_CUDA

#ifdef TENSORFLOW_USE_NUMA
    int numa_num = port::NUMANumNodes();
    std::vector<unsigned> visible_cpus;
    for (int i = 0; i < numa_num; ++i) {
      std::vector<unsigned> cpus;
      port::NUMANodeCPUs(i, &cpus);
      visible_cpus.insert(visible_cpus.end(), cpus.begin(), cpus.end());
    }
    std::vector<std::vector<unsigned> > visible_cpus_per_session;
    AllocateVisibleCpusForSession(visible_cpus, session_num,
                                  visible_cpus_per_session);
#endif  // TENSORFLOW_USE_NUMA

    // Create shared resource for cpu devices
    ResourceMgr* shared_rmgr = new ResourceMgr("localhost");
    DeviceResourceMgrMap dev_rmgr_map;
    std::string dev_prefix("/job:localhost/replica:0/task:0");
    dev_rmgr_map.device_rmgr_map[dev_prefix+"/device:CPU:0"] = shared_rmgr;
    dev_rmgr_map.device_rmgr_map[dev_prefix+"/device:cpu:0"] = shared_rmgr;
    dev_rmgr_map.device_rmgr_map["/device:CPU:0"] = shared_rmgr;
    dev_rmgr_map.device_rmgr_map["/device:cpu:0"] = shared_rmgr;

    ResourceMgr* gpu_shared_rmgr = nullptr;
#if GOOGLE_CUDA
    if (use_multi_stream) {
      // Create shared resource for gpu devices
      gpu_shared_rmgr = new ResourceMgr("localhost");
      std::string gpu_dev_prefix("/job:localhost/replica:0/task:0/device:GPU:");
      for (int i = 0; i < session_num; ++i) {
        dev_rmgr_map.device_rmgr_map[gpu_dev_prefix+std::to_string(i)] =
            gpu_shared_rmgr;
      }
    }
#endif // GOOGLE_CUDA

    DeviceGlobalThreadPoolOptions dev_global_tp_opt;
    dev_global_tp_opt.global_threadpool_num = session_num;
    dev_global_tp_opt.device_threadpool_index = 0;
    std::vector<std::unique_ptr<Device>> devices;
    TF_RETURN_IF_ERROR(DeviceFactory::AddDevices(
        options, "/job:localhost/replica:0/task:0",
        &devices, &dev_rmgr_map, dev_global_tp_opt));

#if GOOGLE_CUDA
    if (use_multi_stream) {
      RemoveUselessDevice(devices, 0);
    }
#endif // GOOGLE_CUDA
    DeviceMgr* device_mgr = new DeviceMgr(std::move(devices));

    SessionGroup* session_group =
        new DirectSessionGroup(shared_rmgr, gpu_shared_rmgr);
    SessionOptions leader_options = options;
#if GOOGLE_CUDA
    if (use_multi_stream) {
      leader_options.config.add_per_session_devices(
          "/job:localhost/replica:0/task:0/device:GPU:0");
    }
#endif // GOOGLE_CUDA

#ifdef TENSORFLOW_USE_NUMA
    DirectSession* leader_session =
        new DirectSession(leader_options, device_mgr, true, this,
                          visible_cpus_per_session[0]);
#else
    DirectSession* leader_session =
        new DirectSession(leader_options, device_mgr, true, this);
#endif  // TENSORFLOW_USE_NUMA
    session_group->CreateLeaderSession(leader_session);
    for (int i = 1; i < session_num; ++i) {
      dev_global_tp_opt.device_threadpool_index = i;
      std::vector<std::unique_ptr<Device>> dev;
      TF_RETURN_IF_ERROR(DeviceFactory::AddDevices(
          options, "/job:localhost/replica:0/task:0", &dev,
          &dev_rmgr_map, dev_global_tp_opt));
      DeviceMgr* dev_mgr = nullptr;
#if GOOGLE_CUDA
      if (use_multi_stream) {
        RemoveUselessDevice(dev, i);
        dev_mgr = new DeviceMgr(std::move(dev));
      } else {
        // Use the same deivce as leader session, this can't get
        // good performance, so user should set use_multi_stream true
        // in session group mode.
        dev_mgr = device_mgr;
      }
#else
      dev_mgr = new DeviceMgr(std::move(dev));
#endif // GOOGLE_CUDA

      SessionOptions follower_options = options;
#if GOOGLE_CUDA
      if (use_multi_stream) {
        follower_options.config.add_per_session_devices(
            "/job:localhost/replica:0/task:0/device:GPU:"+std::to_string(i));
      }
#endif // GOOGLE_CUDA

#ifdef TENSORFLOW_USE_NUMA
      DirectSession* follower_session =
          new DirectSession(follower_options, dev_mgr, true, this,
                            visible_cpus_per_session[i]);
#else
      DirectSession* follower_session =
          new DirectSession(follower_options, dev_mgr, true, this);
#endif  // TENSORFLOW_USE_NUMA
      session_group->CreateFollowerSession(follower_session);
      {
        mutex_lock l(sessions_lock_);
        sessions_.push_back(follower_session);
      }
    }

    {
      mutex_lock l(sessions_lock_);
      sessions_.push_back(leader_session);
    }
    *out_session_group = session_group;

    return Status::OK();
  }

  Status Reset(const SessionOptions& options,
               const std::vector<string>& containers) override {
    std::vector<DirectSession*> sessions_to_reset;
    {
      mutex_lock l(sessions_lock_);
      // We create a copy to ensure that we don't have a deadlock when
      // session->Close calls the DirectSessionFactory.Deregister, which
      // acquires sessions_lock_.
      std::swap(sessions_to_reset, sessions_);
    }
    Status s;
    for (auto session : sessions_to_reset) {
      s.Update(session->Reset(containers));
    }
    // TODO(suharshs): Change the Reset behavior of all SessionFactories so that
    // it doesn't close the sessions?
    for (auto session : sessions_to_reset) {
      s.Update(session->Close());
    }
    return s;
  }

  void Deregister(const DirectSession* session) {
    mutex_lock l(sessions_lock_);
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session),
                    sessions_.end());
    if (session->options().config.experimental().has_session_metadata()) {
      session_metadata_keys_.erase(GetMetadataKey(
          session->options().config.experimental().session_metadata()));
    }
  }

 private:
  void RemoveUselessDevice(std::vector<std::unique_ptr<Device>>& devices,
                           int stream_idx) {
    std::string base_dev_name("/job:localhost/replica:0/task:0/device:GPU:");
    std::string stream_device(base_dev_name+std::to_string(stream_idx));
    int idx = 0;
    while (idx < devices.size()) {
      // remove useless virtual gpu device
      if (devices[idx]->name().find(base_dev_name) != std::string::npos &&
          devices[idx]->name() != stream_device) {
        devices.erase(devices.begin() + idx);
      } else {
        ++idx;
      }
    }
  }

  static string GetMetadataKey(const SessionMetadata& metadata) {
    return absl::StrCat(metadata.name(), "/", metadata.version());
  }

  mutex sessions_lock_;
  std::vector<DirectSession*> sessions_ GUARDED_BY(sessions_lock_);
  absl::flat_hash_set<string> session_metadata_keys_ GUARDED_BY(sessions_lock_);
};

class DirectSessionRegistrar {
 public:
  DirectSessionRegistrar() {
    SessionFactory::Register("DIRECT_SESSION", new DirectSessionFactory());
  }
};
static DirectSessionRegistrar registrar;

std::atomic_int_fast64_t DirectSession::step_id_counter_(1);

static RunHandlerPool* GetOrCreateRunHandlerPool(
    const SessionOptions& options) {
  int num_inter_threads = 0;
  int num_intra_threads = 0;
  static const int env_num_inter_threads = NumInterOpThreadsFromEnvironment();
  static const int env_num_intra_threads = NumIntraOpThreadsFromEnvironment();
  if (env_num_inter_threads > 0) {
    num_inter_threads = env_num_inter_threads;
  }
  if (env_num_intra_threads > 0) {
    num_intra_threads = env_num_intra_threads;
  }

  if (num_inter_threads == 0) {
    if (options.config.session_inter_op_thread_pool_size() > 0) {
      // Note due to ShouldUseRunHandler we are guaranteed that
      // run_options.inter_op_thread_pool() == 0
      num_inter_threads =
          options.config.session_inter_op_thread_pool(0).num_threads();
    }
    if (num_inter_threads == 0) {
      num_inter_threads = NumInterOpThreadsFromSessionOptions(options);
    }
  }

  if (num_intra_threads == 0) {
    num_intra_threads = options.config.intra_op_parallelism_threads();
    if (num_intra_threads == 0) {
      num_intra_threads = port::MaxParallelism();
    }
  }

  static RunHandlerPool* pool =
      new RunHandlerPool(num_inter_threads, num_intra_threads);
  return pool;
}

bool DirectSession::ShouldUseRunHandlerPool(
    const RunOptions& run_options) const {
  if (options_.config.use_per_session_threads()) return false;
  if (options_.config.session_inter_op_thread_pool_size() > 0 &&
      run_options.inter_op_thread_pool() > 0)
    return false;
  // Only use RunHandlerPool when:
  // a. Single global thread pool is used for inter-op parallelism.
  // b. When multiple inter_op_thread_pool(s) are created, use it only while
  // running sessions on the default inter_op_thread_pool=0. Typically,
  // servo-team uses inter_op_thread_pool > 0 for model loading.
  // TODO(crk): Revisit whether we'd want to create one (static) RunHandlerPool
  // per entry in session_inter_op_thread_pool() in the future.
  return true;
}

DirectSession::DirectSession(const SessionOptions& options,
                             const DeviceMgr* device_mgr,
                             bool owd_device_mgr,
#ifdef TENSORFLOW_USE_NUMA
                             DirectSessionFactory* factory,
                             const std::vector<unsigned>& visible_cpus)
#else
                             DirectSessionFactory* const factory)
#endif
    : options_(options),
      own_device_mgr_(owd_device_mgr),
      device_mgr_(device_mgr),
      factory_(factory),
      cancellation_manager_(new CancellationManager()),
      operation_timeout_in_ms_(options_.config.operation_timeout_in_ms()) {
  const int thread_pool_size =
      options_.config.session_inter_op_thread_pool_size();
  if (thread_pool_size > 0) {
    for (int i = 0; i < thread_pool_size; ++i) {
      thread::ThreadPool* pool = nullptr;
      bool owned = false;
      init_error_.Update(NewThreadPoolFromThreadPoolOptions(
          options_, options_.config.session_inter_op_thread_pool(i), i, &pool,
          &owned));
      thread_pools_.emplace_back(pool, owned);
    }
  } else if (options_.config.use_per_session_threads()) {
    thread_pools_.emplace_back(NewThreadPoolFromSessionOptions(options_),
                               true /* owned */);
  } else {
    thread_pools_.emplace_back(GlobalThreadPool(options), false /* owned */);
    // Run locally if environment value of TF_NUM_INTEROP_THREADS is negative
    // and config.inter_op_parallelism_threads is unspecified or negative.
    static const int env_num_threads = NumInterOpThreadsFromEnvironment();
    if (options_.config.inter_op_parallelism_threads() < 0 ||
        (options_.config.inter_op_parallelism_threads() == 0 &&
         env_num_threads < 0)) {
      run_in_caller_thread_ = true;
    }
    MemoryPlannerFactory::GetMemoryPlanner()->SetThreadPool(GlobalThreadPool(options));
  }

  bool use_cost_model_executor = false;
  bool use_inline_executor = false;
  bool pin_threadpool_to_cpu_core = false;
  Status s =
      ReadBoolFromEnvVar("USE_COST_MODEL_EXECUTOR", false, &use_cost_model_executor);
  if (!s.ok()) {
    LOG(FATAL) << s.error_message();
  }
  s = ReadBoolFromEnvVar("USE_INLINE_EXECUTOR", false, &use_inline_executor);
  if (!s.ok()) {
    LOG(FATAL) << s.error_message();
  }
  s = ReadBoolFromEnvVar("SET_SESSION_THREAD_POOL_AFFINITY", false,
                         &pin_threadpool_to_cpu_core);
  if (!s.ok()) {
    LOG(FATAL) << s.error_message();
  }

  // Select which executor to use
  if (options_.config.executor_policy() ==
      ExecutorPolicy::USE_COST_MODEL_EXECUTOR || use_cost_model_executor) {
    run_cost_model_executor_ = true;
  } else if (options_.config.executor_policy() ==
             ExecutorPolicy::USE_INLINE_EXECUTOR || use_inline_executor) {
    run_in_caller_thread_ = true;
  }

  // The default value of sync_on_finish will be flipped soon and this
  // environment variable will be removed as well.
  const Status status =
      ReadBoolFromEnvVar("TF_SYNC_ON_FINISH", true, &sync_on_finish_);
  if (!status.ok()) {
    LOG(ERROR) << status.error_message();
  }
  session_handle_ =
      strings::StrCat("direct", strings::FpToString(random::New64()));
  int devices_added = 0;
  if (options.config.log_device_placement()) {
    const string mapping_str = device_mgr_->DeviceMappingString();
    if (mapping_str.empty()) {
      printf("Device mapping: no known devices.\n");
    } else {
      printf("Device mapping:\n%s", mapping_str.c_str());
    }
    string msg = strings::StrCat("Device mapping:\n", mapping_str);
    if (!logging::LogToListeners(msg)) {
      LOG(INFO) << msg;
    }
  }
  for (auto d : device_mgr_->ListDevices()) {
    devices_.push_back(d);
    device_set_.AddDevice(d);
    d->op_segment()->AddHold(session_handle_);

    // The first device added is special: it is the 'client device' (a
    // CPU device) from which we feed and fetch Tensors.
    if (devices_added == 0) {
      device_set_.set_client_device(d);
    }
    ++devices_added;
  }

#ifdef TENSORFLOW_USE_NUMA
  // thread pool set affinity
  if (pin_threadpool_to_cpu_core && options_.config.use_per_session_threads()) {
    if (thread_pools_.size() != 1) {
      LOG(FATAL) << "Thread pool num is not 1 with 'use_per_session_threads' option.";
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for(auto c : visible_cpus) {
      CPU_SET(c, &cpuset);
      LOG(INFO) << "Current DirectSession " << this << " will be pinned to core: " << c;
    }
    thread_pools_[0].first->SetThreadPoolAffinity(cpuset);
  }
#endif
}

DirectSession::~DirectSession() {
  if (!closed_) Close().IgnoreError();
  for (auto& it : partial_runs_) {
    it.second.reset(nullptr);
  }
  for (auto& it : executors_) {
    it.second.reset();
  }
  callables_.clear();
  for (auto d : device_mgr_->ListDevices()) {
    d->op_segment()->RemoveHold(session_handle_);
  }
  functions_.clear();
  delete cancellation_manager_;
  for (const auto& p_and_owned : thread_pools_) {
    if (p_and_owned.second) delete p_and_owned.first;
  }

  execution_state_.reset(nullptr);
  flib_def_.reset(nullptr);

  if (own_device_mgr_) {
    delete device_mgr_;
  }
}

Status DirectSession::Create(const GraphDef& graph) {
  return Create(GraphDef(graph));
}

Status DirectSession::Create(GraphDef&& graph) {
  TF_RETURN_IF_ERROR(init_error_);
  if (graph.node_size() > 0) {
    mutex_lock l(graph_state_lock_);
    if (graph_created_) {
      return errors::AlreadyExists(
          "A Graph has already been created for this session.");
    }
    return ExtendLocked(std::move(graph));
  }
  return Status::OK();
}

Status DirectSession::Extend(const GraphDef& graph) {
  return Extend(GraphDef(graph));
}

Status DirectSession::Extend(GraphDef&& graph) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  mutex_lock l(graph_state_lock_);
  return ExtendLocked(std::move(graph));
}

Status DirectSession::ExtendLocked(GraphDef graph) {
  if (!(flib_def_ && execution_state_)) {
    // If this is the first call, we can initialize the execution state
    // with `graph` and do not need to call `Extend()`.
    // NOTE(mrry): The function library created here will be used for
    // all subsequent extensions of the graph.
    flib_def_.reset(
        new FunctionLibraryDefinition(OpRegistry::Global(), graph.library()));
    GraphExecutionStateOptions options;
    options.device_set = &device_set_;
    options.session_options = &options_;
    options.session_handle = session_handle_;
    TF_RETURN_IF_ERROR(GraphExecutionState::MakeForBaseGraph(
        std::move(graph), options, &execution_state_));
    graph_created_ = true;
  } else {
    TF_RETURN_IF_ERROR(flib_def_->AddLibrary(graph.library()));
    std::unique_ptr<GraphExecutionState> state;
    // TODO(mrry): Rewrite GraphExecutionState::Extend() to take `graph` by
    // value and move `graph` in here.
    TF_RETURN_IF_ERROR(execution_state_->Extend(graph, &state));
    execution_state_.swap(state);
  }
  return Status::OK();
}

Status DirectSession::Run(const NamedTensorList& inputs,
                          const std::vector<string>& output_names,
                          const std::vector<string>& target_nodes,
                          std::vector<Tensor>* outputs) {
  RunMetadata run_metadata;
  return Run(RunOptions(), inputs, output_names, target_nodes, outputs,
             &run_metadata);
}

Status DirectSession::CreateDebuggerState(
    const CallableOptions& callable_options, int64 global_step,
    int64 session_run_index, int64 executor_step_index,
    std::unique_ptr<DebuggerStateInterface>* debugger_state) {
  TF_RETURN_IF_ERROR(DebuggerStateRegistry::CreateState(
      callable_options.run_options().debug_options(), debugger_state));
  std::vector<string> input_names(callable_options.feed().begin(),
                                  callable_options.feed().end());
  std::vector<string> output_names(callable_options.fetch().begin(),
                                   callable_options.fetch().end());
  std::vector<string> target_names(callable_options.target().begin(),
                                   callable_options.target().end());

  TF_RETURN_IF_ERROR(debugger_state->get()->PublishDebugMetadata(
      global_step, session_run_index, executor_step_index, input_names,
      output_names, target_names));
  return Status::OK();
}

Status DirectSession::DecorateAndPublishGraphForDebug(
    const DebugOptions& debug_options, Graph* graph, Device* device) {
  std::unique_ptr<DebugGraphDecoratorInterface> decorator;
  TF_RETURN_IF_ERROR(
      DebugGraphDecoratorRegistry::CreateDecorator(debug_options, &decorator));

  TF_RETURN_IF_ERROR(decorator->DecorateGraph(graph, device));
  TF_RETURN_IF_ERROR(decorator->PublishGraph(*graph, device->name()));
  return Status::OK();
}

Status DirectSession::RunInternal(
    int64 step_id, const RunOptions& run_options,
    CallFrameInterface* call_frame, ExecutorsAndKeys* executors_and_keys,
    RunMetadata* run_metadata,
    const thread::ThreadPoolOptions& threadpool_options) {
  const uint64 start_time_usecs = options_.env->NowMicros();
  const int64 executor_step_count = executors_and_keys->step_count.fetch_add(1);
  RunState run_state(step_id, &devices_);

  profiler::TraceMe activity(
      [&] {
        if (options_.config.experimental().has_session_metadata()) {
          const auto& model_metadata =
              options_.config.experimental().session_metadata();
          return strings::StrCat("SessionRun #id=", step_id,
                                 ",model_id=", model_metadata.name(), ":",
                                 model_metadata.version(), "#");
        } else {
          return strings::StrCat("SessionRun #id=", step_id, "#");
        }
      },
      profiler::TraceMeLevel::kInfo);

  std::unique_ptr<DebuggerStateInterface> debugger_state;
  if (!run_options.debug_options().debug_tensor_watch_opts().empty()) {
    TF_RETURN_IF_ERROR(
        CreateDebuggerState(executors_and_keys->callable_options,
                            run_options.debug_options().global_step(), step_id,
                            executor_step_count, &debugger_state));
  }

  run_state.rendez = new IntraProcessRendezvous(device_mgr_);
#ifndef __ANDROID__
  // Set up for collectives if ExecutorsAndKeys declares a key.
  if (executors_and_keys->collective_graph_key !=
      BuildGraphOptions::kNoCollectiveGraphKey) {
    if (run_options.experimental().collective_graph_key() !=
        BuildGraphOptions::kNoCollectiveGraphKey) {
      // If a collective_graph_key was specified in run_options, ensure that it
      // matches what came out of GraphExecutionState::BuildGraph().
      if (run_options.experimental().collective_graph_key() !=
          executors_and_keys->collective_graph_key) {
        return errors::Internal(
            "collective_graph_key in RunOptions ",
            run_options.experimental().collective_graph_key(),
            " should match collective_graph_key from optimized graph ",
            executors_and_keys->collective_graph_key);
      }
    }
    if (!collective_executor_mgr_) {
      std::unique_ptr<DeviceResolverInterface> drl(
          new DeviceResolverLocal(device_mgr_));
      std::unique_ptr<ParamResolverInterface> cprl(
          new CollectiveParamResolverLocal(options_.config, device_mgr_,
                                           drl.get(),
                                           "/job:localhost/replica:0/task:0"));
      collective_executor_mgr_.reset(new CollectiveExecutorMgr(
          options_.config, device_mgr_, std::move(drl), std::move(cprl)));
    }
    run_state.collective_executor.reset(new CollectiveExecutor::Handle(
        collective_executor_mgr_->FindOrCreate(step_id), true /*inherit_ref*/));
  }
#endif

  // Start parallel Executors.
  const size_t num_executors = executors_and_keys->items.size();
  ExecutorBarrier* barrier = new ExecutorBarrier(
      num_executors, run_state.rendez, [&run_state](const Status& ret) {
        {
          mutex_lock l(run_state.mu_);
          run_state.status.Update(ret);
        }
        run_state.executors_done.Notify();
      });

  Executor::Args args;
  args.step_id = step_id;
  args.call_frame = call_frame;
  args.rendezvous = run_state.rendez;
  args.global_rendezvous = run_state.rendez;
  args.collective_executor =
      (run_state.collective_executor ? run_state.collective_executor->get()
                                     : nullptr);
  CancellationManager step_cancellation_manager;
  args.cancellation_manager = &step_cancellation_manager;
  args.session_state = &session_state_;
  args.session_handle = session_handle_;
  args.tensor_store = &run_state.tensor_store;
  args.step_container = &run_state.step_container;
  args.sync_on_finish = sync_on_finish_;
  args.user_intra_op_threadpool = threadpool_options.intra_op_threadpool;
  if (run_in_caller_thread_) {
    args.executor_policy = ExecutorPolicy::USE_INLINE_EXECUTOR;
  } else if (run_cost_model_executor_) {
    args.executor_policy = ExecutorPolicy::USE_COST_MODEL_EXECUTOR;
  } else {
    args.executor_policy = ExecutorPolicy::USE_NORMAL_EXECUTOR;
  }

  const bool do_trace = (run_options.trace_level() > RunOptions::NO_TRACE);

  bool update_cost_model = false;
  if (options_.config.graph_options().build_cost_model() > 0) {
    const int64 build_cost_model_every =
        options_.config.graph_options().build_cost_model();
    const int64 build_cost_model_after =
        options_.config.graph_options().build_cost_model_after();
    int64 measure_step_count = executor_step_count - build_cost_model_after;
    if (measure_step_count >= 0) {
      update_cost_model =
          ((measure_step_count + 1) % build_cost_model_every == 0);
    }
  }
  if (do_trace || update_cost_model ||
      run_options.report_tensor_allocations_upon_oom()) {
    run_state.collector.reset(
        new StepStatsCollector(run_metadata->mutable_step_stats()));
    args.stats_collector = run_state.collector.get();
  }

  std::unique_ptr<ProfilerSession> profiler_session;
  if (run_options.trace_level() >= RunOptions::HARDWARE_TRACE) {
    profiler_session = ProfilerSession::Create();
  }

  if (run_options.inter_op_thread_pool() < -1 ||
      run_options.inter_op_thread_pool() >=
          static_cast<int32>(thread_pools_.size())) {
    run_state.executors_done.Notify();
    delete barrier;
    return errors::InvalidArgument("Invalid inter_op_thread_pool: ",
                                   run_options.inter_op_thread_pool());
  }

  // Register this step with session's cancellation manager, so that
  // `Session::Close()` will cancel the step.
  const CancellationToken cancellation_token =
      cancellation_manager_->get_cancellation_token();
  const bool already_cancelled = !cancellation_manager_->RegisterCallback(
      cancellation_token, [&step_cancellation_manager]() {
        step_cancellation_manager.StartCancel();
      });
  if (already_cancelled) {
    // NOTE(mrry): If we don't explicitly notify
    // `run_state.executors_done`, the RunState destructor would
    // block on this notification.
    run_state.executors_done.Notify();
    delete barrier;
    return errors::Cancelled("Run call was cancelled");
  }

  // Use std::unique_ptr to ensure garbage collection
  std::unique_ptr<thread::ThreadPool> threadpool_wrapper;
  thread::ThreadPool* pool = nullptr;

  if (run_in_caller_thread_) {
    pool = nullptr;
  } else if (threadpool_options.inter_op_threadpool != nullptr) {
    threadpool_wrapper = absl::make_unique<thread::ThreadPool>(
        threadpool_options.inter_op_threadpool);
    pool = threadpool_wrapper.get();
  } else if (run_options.inter_op_thread_pool() >= 0) {
    pool = thread_pools_[run_options.inter_op_thread_pool()].first;
  }

  if (pool == nullptr) {
    // We allow using the caller thread only when having a single executor
    // specified.
    if (executors_and_keys->items.size() > 1) {
      pool = thread_pools_[0].first;
    } else {
      VLOG(1) << "Executing Session::Run() synchronously!";
    }
  }

  std::unique_ptr<RunHandler> handler;
  if (ShouldUseRunHandlerPool(run_options) &&
      run_options.experimental().use_run_handler_pool()) {
    VLOG(1) << "Using RunHandler to scheduler inter-op closures.";
    handler = GetOrCreateRunHandlerPool(options_)->Get(step_id);
  }
  auto* handler_ptr = handler.get();

  Executor::Args::Runner default_runner = nullptr;
  // CostRunner will schedule ops according cost model.
  Executor::Args::CostRunner default_cost_runner = nullptr;

  if (pool == nullptr) {
    default_runner = [](Executor::Args::Closure c) { c(); };
    default_cost_runner = [](Executor::Args::Closure c, int64 cost) { c(); };
  } else if (handler_ptr != nullptr) {
    default_runner = [handler_ptr](Executor::Args::Closure c) {
      handler_ptr->ScheduleInterOpClosure(std::move(c));
    };
    // TODO: Consider RunHandlePool cost schedule.
    default_cost_runner = [handler_ptr](Executor::Args::Closure c, int64 cost) {
      handler_ptr->ScheduleInterOpClosure(std::move(c));
    };
  } else {
    default_runner = [this, pool](Executor::Args::Closure c) {
      pool->Schedule(std::move(c));
    };
    default_cost_runner = [this, pool](Executor::Args::Closure c, int64 cost) {
      pool->CostSchedule(std::move(c), cost);
    };
  }

  for (const auto& item : executors_and_keys->items) {
    // TODO(azaks): support partial run.
    // TODO(azaks): if the device picks its own threadpool, we need to assign
    //     less threads to the main compute pool by default.
    thread::ThreadPool* device_thread_pool =
        item.device->tensorflow_device_thread_pool();
    // TODO(crk): Investigate usage of RunHandlerPool when using device specific
    // thread pool(s).
    if (!device_thread_pool) {
      args.runner = default_runner;
      args.cost_runner = default_cost_runner;
    } else {
      args.runner = [this, device_thread_pool](Executor::Args::Closure c) { 
        device_thread_pool->Schedule(std::move(c));
      };
      args.cost_runner = [this, device_thread_pool](Executor::Args::Closure c, int64 cost) { 
        device_thread_pool->Schedule(std::move(c));
      };
    }
    if (handler != nullptr) {
      args.user_intra_op_threadpool = handler->AsIntraThreadPoolInterface();
    }

    item.executor->RunAsync(args, barrier->Get());
  }

  WaitForNotification(&run_state, &step_cancellation_manager,
                      run_options.timeout_in_ms() > 0
                          ? run_options.timeout_in_ms()
                          : operation_timeout_in_ms_);

  if (!cancellation_manager_->DeregisterCallback(cancellation_token)) {
    // The step has been cancelled: make sure we don't attempt to receive the
    // outputs as this would make it block forever.
    mutex_lock l(run_state.mu_);
    run_state.status.Update(errors::Cancelled("Run call was cancelled"));
  }

  if (profiler_session) {
    TF_RETURN_IF_ERROR(profiler_session->CollectData(run_metadata));
  }

  {
    mutex_lock l(run_state.mu_);
    TF_RETURN_IF_ERROR(run_state.status);
  }

  // Save the output tensors of this run we choose to keep.
  if (!run_state.tensor_store.empty()) {
    TF_RETURN_IF_ERROR(run_state.tensor_store.SaveTensors(
        {executors_and_keys->callable_options.fetch().begin(),
         executors_and_keys->callable_options.fetch().end()},
        &session_state_));
  }

  if (run_state.collector) {
    run_state.collector->Finalize();
  }

  // Build and return the cost model as instructed.
  if (update_cost_model) {
    // Build the cost model
    std::unordered_map<string, const Graph*> device_to_graph;
    for (const PerPartitionExecutorsAndLib& partition :
         executors_and_keys->items) {
      const Graph* graph = partition.graph;
      const string device = partition.flib->device()->name();
      device_to_graph[device] = graph;
    }

    mutex_lock l(executor_lock_);
    run_state.collector->BuildCostModel(&cost_model_manager_, device_to_graph);

    // annotate stats onto cost graph.
    CostGraphDef* cost_graph = run_metadata->mutable_cost_graph();
    for (const auto& item : executors_and_keys->items) {
      TF_RETURN_IF_ERROR(
          cost_model_manager_.AddToCostGraphDef(item.graph, cost_graph));
    }
  }

  // If requested via RunOptions, output the partition graphs.
  if (run_options.output_partition_graphs()) {
    protobuf::RepeatedPtrField<GraphDef>* partition_graph_defs =
        run_metadata->mutable_partition_graphs();
    for (const PerPartitionExecutorsAndLib& exec_and_lib :
         executors_and_keys->items) {
      GraphDef* partition_graph_def = partition_graph_defs->Add();
      exec_and_lib.graph->ToGraphDef(partition_graph_def);
    }
  }
  metrics::UpdateGraphExecTime(options_.env->NowMicros() - start_time_usecs);

  return Status::OK();
}

bool DirectSession::EnableTensorPoolTracking(ExecutorsAndKeys* executors_and_keys) {
  static std::unordered_map<ExecutorsAndKeys*, bool> has_training_graph;
  if (has_training_graph.find(executors_and_keys) == has_training_graph.end()) {
    for (const PerPartitionExecutorsAndLib& partition :
        executors_and_keys->items) {
      if (partition.graph->IsTrainingGraph()) {
        has_training_graph[executors_and_keys] = true;
        return true;
      }
    }
    has_training_graph[executors_and_keys] = false;
  }
  return has_training_graph[executors_and_keys];
}

Status DirectSession::Run(const RunOptions& run_options,
                          const NamedTensorList& inputs,
                          const std::vector<string>& output_names,
                          const std::vector<string>& target_nodes,
                          std::vector<Tensor>* outputs,
                          RunMetadata* run_metadata) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  TF_RETURN_IF_ERROR(CheckGraphCreated("Run()"));
  direct_session_runs->GetCell()->IncrementBy(1);

  ScopedMemoryCollector scoped_memory_collector;
  std::unique_ptr<GPUScopedMemoryCollector> scoped_memory_collector_gpu_ptr;

  // Extract the inputs names for this run of the session.
  std::vector<string> input_tensor_names;
  input_tensor_names.reserve(inputs.size());
  size_t input_size = 0;
  for (const auto& it : inputs) {
    input_tensor_names.push_back(it.first);
    input_size += it.second.AllocatedBytes();
  }
  metrics::RecordGraphInputTensors(input_size);

  // Check if we already have an executor for these arguments.
  ExecutorsAndKeys* executors_and_keys;
  RunStateArgs run_state_args(run_options.debug_options());
  run_state_args.collective_graph_key =
      run_options.experimental().collective_graph_key();

  TF_RETURN_IF_ERROR(GetOrCreateExecutors(input_tensor_names, output_names,
                                          target_nodes, &executors_and_keys,
                                          &run_state_args));
  {
    mutex_lock l(collective_graph_key_lock_);
    collective_graph_key_ = executors_and_keys->collective_graph_key;
    if (EnableTensorPoolTracking(executors_and_keys)) {
      scoped_memory_collector_gpu_ptr.reset(new GPUScopedMemoryCollector);
    }
  }

  // Configure a call frame for the step, which we use to feed and
  // fetch values to and from the executors.
  FunctionCallFrame call_frame(executors_and_keys->input_types,
                               executors_and_keys->output_types);
  gtl::InlinedVector<Tensor, 4> feed_args(inputs.size());
  for (const auto& it : inputs) {
    if (it.second.dtype() == DT_RESOURCE) {
      Tensor tensor_from_handle;
      TF_RETURN_IF_ERROR(
          ResourceHandleToInputTensor(it.second, &tensor_from_handle));
      feed_args[executors_and_keys->input_name_to_index[it.first]] =
          tensor_from_handle;
    } else {
      feed_args[executors_and_keys->input_name_to_index[it.first]] = it.second;
    }
  }
  const Status s = call_frame.SetArgs(feed_args);
  if (errors::IsInternal(s)) {
    return errors::InvalidArgument(s.error_message());
  } else if (!s.ok()) {
    return s;
  }

  const int64 step_id = step_id_counter_.fetch_add(1);

  if (LogMemory::IsEnabled()) {
    LogMemory::RecordStep(step_id, run_state_args.handle);
  }

  TF_RETURN_IF_ERROR(RunInternal(step_id, run_options, &call_frame,
                                 executors_and_keys, run_metadata,
                                 thread::ThreadPoolOptions()));

  // Receive outputs.
  if (outputs) {
    std::vector<Tensor> sorted_outputs;
    const Status s = call_frame.ConsumeRetvals(
        &sorted_outputs, /* allow_dead_tensors = */ false);
    if (errors::IsInternal(s)) {
      return errors::InvalidArgument(s.error_message());
    } else if (!s.ok()) {
      return s;
    }
    const bool unique_outputs =
        output_names.size() == executors_and_keys->output_name_to_index.size();
    // first_indices[i] = j implies that j is the smallest value for which
    // output_names[i] == output_names[j].
    std::vector<int> first_indices;
    if (!unique_outputs) {
      first_indices.resize(output_names.size());
      for (int i = 0; i < output_names.size(); ++i) {
        for (int j = 0; j <= i; ++j) {
          if (output_names[i] == output_names[j]) {
            first_indices[i] = j;
            break;
          }
        }
      }
    }
    outputs->clear();
    size_t output_size = 0;
    outputs->reserve(sorted_outputs.size());
    for (int i = 0; i < output_names.size(); ++i) {
      const string& output_name = output_names[i];
      if (first_indices.empty() || first_indices[i] == i) {
        outputs->emplace_back(
            std::move(sorted_outputs[executors_and_keys
                                         ->output_name_to_index[output_name]]));
      } else {
        outputs->push_back((*outputs)[first_indices[i]]);
      }
      output_size += outputs->back().AllocatedBytes();
    }
    metrics::RecordGraphOutputTensors(output_size);
  }

  return Status::OK();
}

Status DirectSession::PRunSetup(const std::vector<string>& input_names,
                                const std::vector<string>& output_names,
                                const std::vector<string>& target_nodes,
                                string* handle) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  TF_RETURN_IF_ERROR(CheckGraphCreated("PRunSetup()"));

  // RunOptions is not available in PRunSetup, so use thread pool 0.
  thread::ThreadPool* pool = thread_pools_[0].first;

  // Check if we already have an executor for these arguments.
  ExecutorsAndKeys* executors_and_keys;
  // TODO(cais): TFDBG support for partial runs.
  DebugOptions debug_options;
  RunStateArgs run_state_args(debug_options);
  run_state_args.is_partial_run = true;
  TF_RETURN_IF_ERROR(GetOrCreateExecutors(input_names, output_names,
                                          target_nodes, &executors_and_keys,
                                          &run_state_args));

  // Create the run state and save it for future PRun calls.
  Executor::Args args;
  if (run_in_caller_thread_) {
    args.executor_policy = ExecutorPolicy::USE_INLINE_EXECUTOR;
  } else if (run_cost_model_executor_) {
    args.executor_policy = ExecutorPolicy::USE_COST_MODEL_EXECUTOR;
  } else {
    args.executor_policy = ExecutorPolicy::USE_NORMAL_EXECUTOR;
  }
  args.step_id = step_id_counter_.fetch_add(1);
  RunState* run_state =
      new RunState(input_names, output_names, args.step_id, &devices_);
  run_state->rendez = new IntraProcessRendezvous(device_mgr_);
  {
    mutex_lock l(executor_lock_);
    if (!partial_runs_
             .emplace(run_state_args.handle,
                      std::unique_ptr<RunState>(run_state))
             .second) {
      return errors::Internal("The handle '", run_state_args.handle,
                              "' created for this partial run is not unique.");
    }
  }

  // Start parallel Executors.
  const size_t num_executors = executors_and_keys->items.size();
  ExecutorBarrier* barrier = new ExecutorBarrier(
      num_executors, run_state->rendez, [run_state](const Status& ret) {
        if (!ret.ok()) {
          mutex_lock l(run_state->mu_);
          run_state->status.Update(ret);
        }
        run_state->executors_done.Notify();
      });

  args.rendezvous = run_state->rendez;
  args.global_rendezvous = run_state->rendez;
  args.cancellation_manager = cancellation_manager_;
  // Note that Collectives are not supported in partial runs
  // because RunOptions is not passed in so we can't know whether
  // their use is intended.
  args.collective_executor = nullptr;
  args.runner = [this, pool](Executor::Args::Closure c) {
    pool->Schedule(std::move(c));
  };
  args.cost_runner = [this, pool](Executor::Args::Closure c, int64 cost) {
    pool->CostSchedule(std::move(c), cost);
  };
  args.session_state = &session_state_;
  args.session_handle = session_handle_;
  args.tensor_store = &run_state->tensor_store;
  args.step_container = &run_state->step_container;
  if (LogMemory::IsEnabled()) {
    LogMemory::RecordStep(args.step_id, run_state_args.handle);
  }
  args.sync_on_finish = sync_on_finish_;

  if (options_.config.graph_options().build_cost_model()) {
    run_state->collector.reset(new StepStatsCollector(nullptr));
    args.stats_collector = run_state->collector.get();
  }

  for (auto& item : executors_and_keys->items) {
    item.executor->RunAsync(args, barrier->Get());
  }

  *handle = run_state_args.handle;
  return Status::OK();
}

Status DirectSession::PRun(const string& handle, const NamedTensorList& inputs,
                           const std::vector<string>& output_names,
                           std::vector<Tensor>* outputs) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  std::vector<string> parts = str_util::Split(handle, ';');
  const string& key = parts[0];
  // Get the executors for this partial run.
  ExecutorsAndKeys* executors_and_keys;
  RunState* run_state;
  {
    mutex_lock l(executor_lock_);  // could use reader lock
    auto exc_it = executors_.find(key);
    if (exc_it == executors_.end()) {
      return errors::InvalidArgument(
          "Must run 'setup' before performing partial runs!");
    }
    executors_and_keys = exc_it->second.get();

    auto prun_it = partial_runs_.find(handle);
    if (prun_it == partial_runs_.end()) {
      return errors::InvalidArgument(
          "Must run 'setup' before performing partial runs!");
    }
    run_state = prun_it->second.get();

    // Make sure that this is a new set of feeds that are still pending.
    for (const auto& input : inputs) {
      auto it = run_state->pending_inputs.find(input.first);
      if (it == run_state->pending_inputs.end()) {
        return errors::InvalidArgument(
            "The feed ", input.first,
            " was not specified in partial_run_setup.");
      } else if (it->second) {
        return errors::InvalidArgument("The feed ", input.first,
                                       " has already been fed.");
      }
    }
    // Check that this is a new set of fetches that are still pending.
    for (const auto& output : output_names) {
      auto it = run_state->pending_outputs.find(output);
      if (it == run_state->pending_outputs.end()) {
        return errors::InvalidArgument(
            "The fetch ", output, " was not specified in partial_run_setup.");
      } else if (it->second) {
        return errors::InvalidArgument("The fetch ", output,
                                       " has already been fetched.");
      }
    }
  }

  // Check that this new set of fetches can be computed from all the
  // feeds we have supplied.
  TF_RETURN_IF_ERROR(
      CheckFetch(inputs, output_names, executors_and_keys, run_state));

  // Send inputs.
  Status s = SendPRunInputs(inputs, executors_and_keys, run_state->rendez);

  // Receive outputs.
  if (s.ok()) {
    s = RecvPRunOutputs(output_names, executors_and_keys, run_state, outputs);
  }

  // Save the output tensors of this run we choose to keep.
  if (s.ok()) {
    s = run_state->tensor_store.SaveTensors(output_names, &session_state_);
  }

  {
    mutex_lock l(executor_lock_);
    // Delete the run state if there is an error or all fetches are done.
    bool done = true;
    if (s.ok()) {
      {
        mutex_lock l(run_state->mu_);
        if (!run_state->status.ok()) {
          LOG(WARNING) << "An error unrelated to this prun has been detected. "
                       << run_state->status;
        }
      }
      for (const auto& input : inputs) {
        auto it = run_state->pending_inputs.find(input.first);
        it->second = true;
      }
      for (const auto& name : output_names) {
        auto it = run_state->pending_outputs.find(name);
        it->second = true;
      }
      done = run_state->PendingDone();
    }
    if (done) {
      WaitForNotification(run_state, cancellation_manager_,
                          operation_timeout_in_ms_);
      partial_runs_.erase(handle);
    }
  }

  return s;
}

Status DirectSession::ResourceHandleToInputTensor(const Tensor& resource_tensor,
                                                  Tensor* retrieved_tensor) {
  if (resource_tensor.dtype() != DT_RESOURCE) {
    return errors::InvalidArgument(strings::StrCat(
        "ResourceHandleToInputTensor() received non-DT_RESOURCE Tensor: ",
        resource_tensor.dtype()));
  }

  const ResourceHandle& resource_handle =
      resource_tensor.scalar<ResourceHandle>()();

  if (resource_handle.container() ==
      SessionState::kTensorHandleResourceTypeName) {
    return session_state_.GetTensor(resource_handle.name(), retrieved_tensor);
  } else {
    return errors::InvalidArgument(strings::StrCat(
        "Invalid resource type hash code: ", resource_handle.hash_code(),
        "(name: ", resource_handle.name(),
        " type: ", resource_handle.maybe_type_name(),
        "). Perhaps a resource tensor was being provided as a feed? That is "
        "not currently allowed. Please file an issue at "
        "https://github.com/tensorflow/tensorflow/issues/new, ideally with a "
        "short code snippet that leads to this error message."));
  }
}

Status DirectSession::SendPRunInputs(const NamedTensorList& inputs,
                                     const ExecutorsAndKeys* executors_and_keys,
                                     IntraProcessRendezvous* rendez) {
  Status s;
  Rendezvous::ParsedKey parsed;
  // Insert the input tensors into the local rendezvous by their
  // rendezvous key.
  for (const auto& input : inputs) {
    auto it =
        executors_and_keys->input_name_to_rendezvous_key.find(input.first);
    if (it == executors_and_keys->input_name_to_rendezvous_key.end()) {
      return errors::Internal("'", input.first, "' is not a pre-defined feed.");
    }
    const string& input_key = it->second;

    s = Rendezvous::ParseKey(input_key, &parsed);
    if (!s.ok()) {
      rendez->StartAbort(s);
      return s;
    }

    if (input.second.dtype() == DT_RESOURCE) {
      Tensor tensor_from_handle;
      s = ResourceHandleToInputTensor(input.second, &tensor_from_handle);
      if (s.ok()) {
        s = rendez->Send(parsed, Rendezvous::Args(), tensor_from_handle, false);
      }
    } else {
      s = rendez->Send(parsed, Rendezvous::Args(), input.second, false);
    }

    if (!s.ok()) {
      rendez->StartAbort(s);
      return s;
    }
  }
  return Status::OK();
}

Status DirectSession::RecvPRunOutputs(
    const std::vector<string>& output_names,
    const ExecutorsAndKeys* executors_and_keys, RunState* run_state,
    std::vector<Tensor>* outputs) {
  Status s;
  if (!output_names.empty()) {
    outputs->resize(output_names.size());
  }

  Rendezvous::ParsedKey parsed;
  // Get the outputs from the rendezvous
  for (size_t output_offset = 0; output_offset < output_names.size();
       ++output_offset) {
    const string& output_name = output_names[output_offset];
    auto it =
        executors_and_keys->output_name_to_rendezvous_key.find(output_name);
    if (it == executors_and_keys->output_name_to_rendezvous_key.end()) {
      return errors::Internal("'", output_name,
                              "' is not a pre-defined fetch.");
    }
    const string& output_key = it->second;
    Tensor output_tensor;
    bool is_dead;
    IntraProcessRendezvous* rendez = run_state->rendez;

    s = Rendezvous::ParseKey(output_key, &parsed);
    if (s.ok()) {
      // Fetch data from the Rendezvous.
      s = rendez->Recv(parsed, Rendezvous::Args(), &output_tensor, &is_dead,
                       operation_timeout_in_ms_);
      if (is_dead && s.ok()) {
        s = errors::InvalidArgument("The tensor returned for ", output_name,
                                    " was not valid.");
      }
    }
    if (!s.ok()) {
      rendez->StartAbort(s);
      outputs->clear();
      return s;
    }

    (*outputs)[output_offset] = output_tensor;
  }
  return Status::OK();
}

Status DirectSession::CheckFetch(const NamedTensorList& feeds,
                                 const std::vector<string>& fetches,
                                 const ExecutorsAndKeys* executors_and_keys,
                                 const RunState* run_state) {
  const Graph* graph = executors_and_keys->graph.get();
  const NameNodeMap* name_to_node = &executors_and_keys->name_to_node;

  // Build the set of pending feeds that we haven't seen.
  std::unordered_set<TensorId, TensorId::Hasher> pending_feeds;
  {
    mutex_lock l(executor_lock_);
    for (const auto& input : run_state->pending_inputs) {
      // Skip if the feed has already been fed.
      if (input.second) continue;
      TensorId id(ParseTensorName(input.first));
      auto it = name_to_node->find(id.first);
      if (it == name_to_node->end()) {
        return errors::NotFound("Feed ", input.first, ": not found");
      }
      pending_feeds.insert(id);
    }
  }
  for (const auto& it : feeds) {
    TensorId id(ParseTensorName(it.first));
    pending_feeds.erase(id);
  }

  // Initialize the stack with the fetch nodes.
  std::vector<const Node*> stack;
  for (const string& fetch : fetches) {
    TensorId id(ParseTensorName(fetch));
    auto it = name_to_node->find(id.first);
    if (it == name_to_node->end()) {
      return errors::NotFound("Fetch ", fetch, ": not found");
    }
    stack.push_back(it->second);
  }

  // Any tensor needed for fetches can't be in pending_feeds.
  std::vector<bool> visited(graph->num_node_ids(), false);
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();

    for (const Edge* in_edge : n->in_edges()) {
      const Node* in_node = in_edge->src();
      if (pending_feeds.count({in_node->name(), in_edge->src_output()}) > 0) {
        return errors::InvalidArgument("Fetch ", in_node->name(), ":",
                                       in_edge->src_output(),
                                       " can't be computed from the feeds"
                                       " that have been fed so far.");
      }
      if (!visited[in_node->id()]) {
        visited[in_node->id()] = true;
        stack.push_back(in_node);
      }
    }
  }
  return Status::OK();
}

Status DirectSession::CreateExecutors(
    const CallableOptions& callable_options,
    std::unique_ptr<ExecutorsAndKeys>* out_executors_and_keys,
    std::unique_ptr<FunctionInfo>* out_func_info,
    RunStateArgs* run_state_args) {
  BuildGraphOptions options;
  options.callable_options = callable_options;
  options.use_function_convention = !run_state_args->is_partial_run;
  options.collective_graph_key =
      callable_options.run_options().experimental().collective_graph_key();
  if (options_.config.experimental()
          .collective_deterministic_sequential_execution()) {
    options.collective_order = GraphCollectiveOrder::kEdges;
  } else if (options_.config.experimental().collective_nccl()) {
    options.collective_order = GraphCollectiveOrder::kAttrs;
  }

  std::unique_ptr<FunctionInfo> func_info(new FunctionInfo);
  std::unique_ptr<ExecutorsAndKeys> ek(new ExecutorsAndKeys);

  ek->callable_options = callable_options;

  std::unordered_map<string, std::unique_ptr<Graph>> graphs;
  TF_RETURN_IF_ERROR(CreateGraphs(
      options, &graphs, &func_info->flib_def, run_state_args, &ek->input_types,
      &ek->output_types, &ek->collective_graph_key));

  if (run_state_args->is_partial_run) {
    ek->graph = std::move(run_state_args->graph);
    std::unordered_set<StringPiece, StringPieceHasher> names;
    for (const string& input : callable_options.feed()) {
      TensorId id(ParseTensorName(input));
      names.emplace(id.first);
    }
    for (const string& output : callable_options.fetch()) {
      TensorId id(ParseTensorName(output));
      names.emplace(id.first);
    }
    for (Node* n : ek->graph->nodes()) {
      if (names.count(n->name()) > 0) {
        ek->name_to_node.insert({n->name(), n});
      }
    }
  }
  ek->items.reserve(graphs.size());
  const auto& optimizer_opts =
      options_.config.graph_options().optimizer_options();

  int graph_def_version = graphs.begin()->second->versions().producer();

  const auto* session_metadata =
      options_.config.experimental().has_session_metadata()
          ? &options_.config.experimental().session_metadata()
          : nullptr;
  func_info->proc_flr.reset(new ProcessFunctionLibraryRuntime(
      device_mgr_, options_.env, graph_def_version,
      func_info->flib_def.get(), optimizer_opts, thread_pools_[0].first,
      nullptr, nullptr, session_metadata));

  GraphOptimizer optimizer(optimizer_opts);
  for (auto iter = graphs.begin(); iter != graphs.end(); ++iter) {
    const string& partition_name = iter->first;
    std::unique_ptr<Graph>& partition_graph = iter->second;

    Device* device;
    TF_RETURN_IF_ERROR(device_mgr_->LookupDevice(partition_name, &device));

    ek->items.resize(ek->items.size() + 1);
    auto* item = &(ek->items.back());
    auto lib = func_info->proc_flr->GetFLR(partition_name);
    if (lib == nullptr) {
      return errors::Internal("Could not find device: ", partition_name);
    }
    item->flib = lib;

    LocalExecutorParams params;
    params.device = device;
    params.session_metadata = session_metadata;
    params.function_library = lib;
    auto opseg = device->op_segment();
    params.create_kernel = [this, lib, opseg](const NodeDef& ndef,
                                              OpKernel** kernel) {
      // NOTE(mrry): We must not share function kernels (implemented
      // using `CallOp`) between subgraphs, because `CallOp::handle_`
      // is tied to a particular subgraph. Even if the function itself
      // is stateful, the `CallOp` that invokes it is not.
      if (!OpSegment::ShouldOwnKernel(lib, ndef.op())) {
        return lib->CreateKernel(ndef, kernel);
      }
      auto create_fn = [lib, &ndef](OpKernel** kernel) {
        return lib->CreateKernel(ndef, kernel);
      };
      // Kernels created for subgraph nodes need to be cached.  On
      // cache miss, create_fn() is invoked to create a kernel based
      // on the function library here + global op registry.
      return opseg->FindOrCreate(session_handle_, ndef.name(), kernel,
                                 create_fn);
    };
    params.delete_kernel = [lib](OpKernel* kernel) {
      if (kernel && !OpSegment::ShouldOwnKernel(lib, kernel->type_string()))
        delete kernel;
    };
    params.rendezvous_factory = [](const int64, const DeviceMgr* device_mgr,
                                   Rendezvous** r) {
      *r = new IntraProcessRendezvous(device_mgr);
      return Status::OK();
    };

    optimizer.Optimize(lib, options_.env, device, &partition_graph,
                       /*shape_map=*/nullptr);

    // TensorFlow Debugger (tfdbg) inserts debug nodes in the graph.
    const DebugOptions& debug_options =
        options.callable_options.run_options().debug_options();
    if (!debug_options.debug_tensor_watch_opts().empty()) {
      TF_RETURN_IF_ERROR(DecorateAndPublishGraphForDebug(
          debug_options, partition_graph.get(), params.device));
    }

    TF_RETURN_IF_ERROR(EnsureMemoryTypes(DeviceType(device->device_type()),
                                         device->name(),
                                         partition_graph.get()));
    // NewLocalExecutor takes ownership of partition_graph.
    item->graph = partition_graph.get();
    item->executor = nullptr;
    item->device = device;
    auto executor_type = options_.config.experimental().executor_type();
    TF_RETURN_IF_ERROR(NewExecutor(
        executor_type, params, std::move(partition_graph), &item->executor));
  }

  // Cache the mapping from input/output names to graph elements to
  // avoid recomputing it every time.
  if (!run_state_args->is_partial_run) {
    // For regular `Run()`, we use the function calling convention, and so
    // maintain a mapping from input/output names to
    // argument/return-value ordinal index.
    for (int i = 0; i < callable_options.feed().size(); ++i) {
      const string& input = callable_options.feed(i);
      ek->input_name_to_index[input] = i;
    }
    for (int i = 0; i < callable_options.fetch().size(); ++i) {
      const string& output = callable_options.fetch(i);
      ek->output_name_to_index[output] = i;
    }
  } else {
    // For `PRun()`, we use the rendezvous calling convention, and so
    // maintain a mapping from input/output names to rendezvous keys.
    //
    // We always use the first device as the device name portion of the
    // key, even if we're feeding another graph.
    for (int i = 0; i < callable_options.feed().size(); ++i) {
      const string& input = callable_options.feed(i);
      ek->input_name_to_rendezvous_key[input] = GetRendezvousKey(
          input, device_set_.client_device()->attributes(), FrameAndIter(0, 0));
    }
    for (int i = 0; i < callable_options.fetch().size(); ++i) {
      const string& output = callable_options.fetch(i);
      ek->output_name_to_rendezvous_key[output] =
          GetRendezvousKey(output, device_set_.client_device()->attributes(),
                           FrameAndIter(0, 0));
    }
  }

  *out_executors_and_keys = std::move(ek);
  *out_func_info = std::move(func_info);
  return Status::OK();
}

Status DirectSession::GetOrCreateExecutors(
    gtl::ArraySlice<string> inputs, gtl::ArraySlice<string> outputs,
    gtl::ArraySlice<string> target_nodes, ExecutorsAndKeys** executors_and_keys,
    RunStateArgs* run_state_args) {
  int64 handle_name_counter_value = -1;
  if (LogMemory::IsEnabled() || run_state_args->is_partial_run) {
    handle_name_counter_value = handle_name_counter_.fetch_add(1);
  }

  string debug_tensor_watches_summary;
  if (!run_state_args->debug_options.debug_tensor_watch_opts().empty()) {
    debug_tensor_watches_summary = SummarizeDebugTensorWatches(
        run_state_args->debug_options.debug_tensor_watch_opts());
  }

  // Fast lookup path, no sorting.
  const string key = strings::StrCat(
      absl::StrJoin(inputs, ","), "->", absl::StrJoin(outputs, ","), "/",
      absl::StrJoin(target_nodes, ","), "/", run_state_args->is_partial_run,
      "/", debug_tensor_watches_summary);
  // Set the handle, if it's needed to log memory or for partial run.
  if (handle_name_counter_value >= 0) {
    run_state_args->handle =
        strings::StrCat(key, ";", handle_name_counter_value);
  }

  // See if we already have the executors for this run.
  {
    mutex_lock l(executor_lock_);  // could use reader lock
    auto it = executors_.find(key);
    if (it != executors_.end()) {
      *executors_and_keys = it->second.get();
      return Status::OK();
    }
  }

  // Slow lookup path, the unsorted key missed the cache.
  // Sort the inputs and outputs, and look up with the sorted key in case an
  // earlier call used a different order of inputs and outputs.
  //
  // We could consider some other signature instead of sorting that
  // preserves the same property to avoid the sort in the future.
  std::vector<string> inputs_sorted(inputs.begin(), inputs.end());
  std::sort(inputs_sorted.begin(), inputs_sorted.end());
  std::vector<string> outputs_sorted(outputs.begin(), outputs.end());
  std::sort(outputs_sorted.begin(), outputs_sorted.end());
  std::vector<string> tn_sorted(target_nodes.begin(), target_nodes.end());
  std::sort(tn_sorted.begin(), tn_sorted.end());

  const string sorted_key = strings::StrCat(
      absl::StrJoin(inputs_sorted, ","), "->",
      absl::StrJoin(outputs_sorted, ","), "/", absl::StrJoin(tn_sorted, ","),
      "/", run_state_args->is_partial_run, "/", debug_tensor_watches_summary);
  // Set the handle, if its needed to log memory or for partial run.
  if (handle_name_counter_value >= 0) {
    run_state_args->handle =
        strings::StrCat(sorted_key, ";", handle_name_counter_value);
  }

  // See if we already have the executors for this run.
  {
    mutex_lock l(executor_lock_);
    auto it = executors_.find(sorted_key);
    if (it != executors_.end()) {
      *executors_and_keys = it->second.get();
      // Insert this under the original key.
      executors_.emplace(key, it->second);
      return Status::OK();
    }
  }

  // Nothing found, so create the executors and store in the cache.
  // The executor_lock_ is intentionally released while executors are
  // being created.
  CallableOptions callable_options;
  for (const string& input : inputs_sorted) {
    callable_options.add_feed(input);
  }
  for (const string& output : outputs_sorted) {
    callable_options.add_fetch(output);
  }
  for (const string& target : tn_sorted) {
    callable_options.add_target(target);
  }
  *callable_options.mutable_run_options()->mutable_debug_options() =
      run_state_args->debug_options;
  callable_options.mutable_run_options()
      ->mutable_experimental()
      ->set_collective_graph_key(run_state_args->collective_graph_key);
  std::unique_ptr<ExecutorsAndKeys> ek;
  std::unique_ptr<FunctionInfo> func_info;
  TF_RETURN_IF_ERROR(
      CreateExecutors(callable_options, &ek, &func_info, run_state_args));

  // Reacquire the lock, try to insert into the map.
  mutex_lock l(executor_lock_);

  // Another thread may have created the entry before us, in which case we will
  // reuse the already created one.
  auto insert_result = executors_.emplace(
      sorted_key, std::shared_ptr<ExecutorsAndKeys>(std::move(ek)));
  if (insert_result.second) {
    functions_.push_back(std::move(func_info));
  }

  // Insert the value under the original key, so the fast path lookup will work
  // if the user uses the same order of inputs, outputs, and targets again.
  executors_.emplace(key, insert_result.first->second);
  *executors_and_keys = insert_result.first->second.get();

  return Status::OK();
}

Status DirectSession::CreateGraphs(
    const BuildGraphOptions& subgraph_options,
    std::unordered_map<string, std::unique_ptr<Graph>>* outputs,
    std::unique_ptr<FunctionLibraryDefinition>* flib_def,
    RunStateArgs* run_state_args, DataTypeVector* input_types,
    DataTypeVector* output_types, int64* collective_graph_key) {
  mutex_lock l(graph_state_lock_);
  std::unique_ptr<ClientGraph> client_graph;

  std::unique_ptr<GraphExecutionState> temp_exec_state_holder;
  GraphExecutionState* execution_state = nullptr;
  if (options_.config.graph_options().place_pruned_graph()) {
    // Because we are placing pruned graphs, we need to create a
    // new GraphExecutionState for every new unseen graph,
    // and then place it.
    GraphExecutionStateOptions prune_options;
    prune_options.device_set = &device_set_;
    prune_options.session_options = &options_;
    prune_options.stateful_placements = stateful_placements_;
    prune_options.session_handle = session_handle_;
    TF_RETURN_IF_ERROR(GraphExecutionState::MakeForPrunedGraph(
        *execution_state_, prune_options, subgraph_options,
        &temp_exec_state_holder, &client_graph));
    execution_state = temp_exec_state_holder.get();
  } else {
    execution_state = execution_state_.get();
    TF_RETURN_IF_ERROR(
        execution_state->BuildGraph(subgraph_options, &client_graph));
  }
  *collective_graph_key = client_graph->collective_graph_key;

  if (subgraph_options.callable_options.feed_size() !=
      client_graph->feed_types.size()) {
    return errors::Internal(
        "Graph pruning failed: requested number of feed endpoints = ",
        subgraph_options.callable_options.feed_size(),
        " versus number of pruned feed endpoints = ",
        client_graph->feed_types.size());
  }
  if (subgraph_options.callable_options.fetch_size() !=
      client_graph->fetch_types.size()) {
    return errors::Internal(
        "Graph pruning failed: requested number of fetch endpoints = ",
        subgraph_options.callable_options.fetch_size(),
        " versus number of pruned fetch endpoints = ",
        client_graph->fetch_types.size());
  }

  auto current_stateful_placements = execution_state->GetStatefulPlacements();
  // Update our current state based on the execution_state's
  // placements.  If there are any mismatches for a node,
  // we should fail, as this should never happen.
  for (auto placement_pair : current_stateful_placements) {
    const string& node_name = placement_pair.first;
    const string& placement = placement_pair.second;
    auto iter = stateful_placements_.find(node_name);
    if (iter == stateful_placements_.end()) {
      stateful_placements_.insert(std::make_pair(node_name, placement));
    } else if (iter->second != placement) {
      return errors::Internal(
          "Stateful placement mismatch. "
          "Current assignment of ",
          node_name, " to ", iter->second, " does not match ", placement);
    }
  }

  stateful_placements_ = execution_state->GetStatefulPlacements();

  // Remember the graph in run state if this is a partial run.
  if (run_state_args->is_partial_run) {
    run_state_args->graph.reset(new Graph(flib_def_.get()));
    CopyGraph(*execution_state->full_graph(), run_state_args->graph.get());
  }

  // Partition the graph across devices.
  PartitionOptions popts;
  popts.node_to_loc = [](const Node* node) {
    return node->assigned_device_name();
  };
  popts.new_name = [this](const string& prefix) {
    return strings::StrCat(prefix, "/_", edge_name_counter_.fetch_add(1));
  };
  popts.get_incarnation = [](const string& name) {
    // The direct session does not have changing incarnation numbers.
    // Just return '1'.
    return 1;
  };
  popts.flib_def = &client_graph->graph.flib_def();
  popts.control_flow_added = false;

  std::unordered_map<string, GraphDef> partitions;
  TF_RETURN_IF_ERROR(Partition(popts, &client_graph->graph, &partitions));

  std::vector<string> device_names;
  for (auto device : devices_) {
    // Extract the LocalName from the device.
    device_names.push_back(DeviceNameUtils::LocalName(device->name()));
  }

  // Check for valid partitions.
  for (const auto& partition : partitions) {
    const string local_partition_name =
        DeviceNameUtils::LocalName(partition.first);
    if (std::count(device_names.begin(), device_names.end(),
                   local_partition_name) == 0) {
      return errors::InvalidArgument(
          "Creating a partition for ", local_partition_name,
          " which doesn't exist in the list of available devices. Available "
          "devices: ",
          absl::StrJoin(device_names, ","));
    }
  }

  for (auto& partition : partitions) {
    std::unique_ptr<Graph> device_graph(
        new Graph(client_graph->flib_def.get()));
    GraphConstructorOptions device_opts;
    // There are internal operations (e.g., send/recv) that we now allow.
    device_opts.allow_internal_ops = true;
    device_opts.expect_device_spec = true;
    TF_RETURN_IF_ERROR(ConvertGraphDefToGraph(
        device_opts, std::move(partition.second), device_graph.get()));
    outputs->emplace(partition.first, std::move(device_graph));
  }

  GraphOptimizationPassOptions optimization_options;
  optimization_options.session_options = &options_;
  optimization_options.flib_def = client_graph->flib_def.get();
  optimization_options.partition_graphs = outputs;
  TF_RETURN_IF_ERROR(OptimizationPassRegistry::Global()->RunGrouping(
      OptimizationPassRegistry::POST_PARTITIONING, optimization_options));

  Status s;
  for (auto& partition : *outputs) {
    const string& partition_name = partition.first;
    std::unique_ptr<Graph>* graph = &partition.second;

    VLOG(2) << "Created " << DebugString(graph->get()) << " for "
            << partition_name;

    // Give the device an opportunity to rewrite its subgraph.
    Device* d;
    s = device_mgr_->LookupDevice(partition_name, &d);
    if (!s.ok()) break;
    s = d->MaybeRewriteGraph(graph);
    if (!s.ok()) {
      break;
    }
  }
  *flib_def = std::move(client_graph->flib_def);
  std::swap(*input_types, client_graph->feed_types);
  std::swap(*output_types, client_graph->fetch_types);
  return s;
}

::tensorflow::Status DirectSession::ListDevices(
    std::vector<DeviceAttributes>* response) {
  response->clear();
  response->reserve(devices_.size());
  for (Device* d : devices_) {
    const DeviceAttributes& attrs = d->attributes();
    response->emplace_back(attrs);
  }
  return ::tensorflow::Status::OK();
}

::tensorflow::Status DirectSession::Reset(
    const std::vector<string>& containers) {
  device_mgr_->ClearContainers(containers);
  return ::tensorflow::Status::OK();
}

::tensorflow::Status DirectSession::Close() {
  cancellation_manager_->StartCancel();
  {
    mutex_lock l(closed_lock_);
    if (closed_) return ::tensorflow::Status::OK();
    closed_ = true;
  }
  if (factory_ != nullptr) factory_->Deregister(this);
  return ::tensorflow::Status::OK();
}

DirectSession::RunState::RunState(
    const std::vector<string>& pending_input_names,
    const std::vector<string>& pending_output_names, int64 step_id,
    const std::vector<Device*>* devices)
    : step_container(step_id, [devices, step_id](const string& name) {
        for (auto d : *devices) {
          if (!d->resource_manager()->Cleanup(name).ok()) {
            // Do nothing...
          }
          ScopedAllocatorMgr* sam = d->GetScopedAllocatorMgr();
          if (sam) sam->Cleanup(step_id);
        }
      }) {
  // Initially all the feeds and fetches are pending.
  for (auto& name : pending_input_names) {
    pending_inputs[name] = false;
  }
  for (auto& name : pending_output_names) {
    pending_outputs[name] = false;
  }
}

DirectSession::RunState::RunState(int64 step_id,
                                  const std::vector<Device*>* devices)
    : RunState({}, {}, step_id, devices) {}

DirectSession::RunState::~RunState() {
  if (rendez != nullptr) {
    if (!executors_done.HasBeenNotified()) {
      rendez->StartAbort(errors::Cancelled("PRun cancellation"));
      executors_done.WaitForNotification();
    }
    rendez->Unref();
  }
}

bool DirectSession::RunState::PendingDone() const {
  for (const auto& it : pending_inputs) {
    if (!it.second) return false;
  }
  for (const auto& it : pending_outputs) {
    if (!it.second) return false;
  }
  return true;
}

void DirectSession::WaitForNotification(RunState* run_state,
                                        CancellationManager* cm,
                                        int64 timeout_in_ms) {
  const Status status =
      WaitForNotification(&run_state->executors_done, timeout_in_ms);
  if (!status.ok()) {
    {
      mutex_lock l(run_state->mu_);
      run_state->status.Update(status);
    }
    cm->StartCancel();
    // We must wait for the executors to complete, because they have borrowed
    // references to `cm` and other per-step state. After this notification, it
    // is safe to clean up the step.
    run_state->executors_done.WaitForNotification();
  }
}

::tensorflow::Status DirectSession::WaitForNotification(
    Notification* notification, int64 timeout_in_ms) {
  if (timeout_in_ms > 0) {
    const int64 timeout_in_us = timeout_in_ms * 1000;
    const bool notified =
        WaitForNotificationWithTimeout(notification, timeout_in_us);
    if (!notified) {
      return Status(error::DEADLINE_EXCEEDED,
                    "Timed out waiting for notification");
    }
  } else {
    notification->WaitForNotification();
  }
  return Status::OK();
}

Status DirectSession::MakeCallable(const CallableOptions& callable_options,
                                   CallableHandle* out_handle) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  TF_RETURN_IF_ERROR(CheckGraphCreated("MakeCallable()"));

  std::unique_ptr<ExecutorsAndKeys> ek;
  std::unique_ptr<FunctionInfo> func_info;
  RunStateArgs run_state_args(callable_options.run_options().debug_options());
  TF_RETURN_IF_ERROR(
      CreateExecutors(callable_options, &ek, &func_info, &run_state_args));
  {
    mutex_lock l(callables_lock_);
    *out_handle = next_callable_handle_++;
    callables_[*out_handle] = {std::move(ek), std::move(func_info)};
  }
  return Status::OK();
}

class DirectSession::RunCallableCallFrame : public CallFrameInterface {
 public:
  RunCallableCallFrame(DirectSession* session,
                       ExecutorsAndKeys* executors_and_keys,
                       const std::vector<Tensor>* feed_tensors,
                       std::vector<Tensor>* fetch_tensors)
      : session_(session),
        executors_and_keys_(executors_and_keys),
        feed_tensors_(feed_tensors),
        fetch_tensors_(fetch_tensors) {}

  size_t num_args() const override {
    return executors_and_keys_->input_types.size();
  }
  size_t num_retvals() const override {
    return executors_and_keys_->output_types.size();
  }

  Status GetArg(int index, Tensor* val) const override {
    if (index > feed_tensors_->size()) {
      return errors::Internal("Args index out of bounds: ", index);
    } else if (executors_and_keys_->input_types[index] == DT_RESOURCE) {
      TF_RETURN_IF_ERROR(
          session_->ResourceHandleToInputTensor((*feed_tensors_)[index], val));
    } else {
      *val = (*feed_tensors_)[index];
    }
    return Status::OK();
  }

  Status SetRetval(int index, const Tensor& val) override {
    if (index > fetch_tensors_->size()) {
      return errors::Internal("RetVal index out of bounds: ", index);
    }
    (*fetch_tensors_)[index] = val;
    return Status::OK();
  }

 private:
  DirectSession* const session_;                   // Not owned.
  ExecutorsAndKeys* const executors_and_keys_;     // Not owned.
  const std::vector<Tensor>* const feed_tensors_;  // Not owned.
  std::vector<Tensor>* const fetch_tensors_;       // Not owned.
};

::tensorflow::Status DirectSession::RunCallable(
    CallableHandle handle, const std::vector<Tensor>& feed_tensors,
    std::vector<Tensor>* fetch_tensors, RunMetadata* run_metadata) {
  return RunCallable(handle, feed_tensors, fetch_tensors, run_metadata,
                     thread::ThreadPoolOptions());
}

::tensorflow::Status DirectSession::RunCallable(
    CallableHandle handle, const std::vector<Tensor>& feed_tensors,
    std::vector<Tensor>* fetch_tensors, RunMetadata* run_metadata,
    const thread::ThreadPoolOptions& threadpool_options) {
  TF_RETURN_IF_ERROR(CheckNotClosed());
  TF_RETURN_IF_ERROR(CheckGraphCreated("RunCallable()"));
  direct_session_runs->GetCell()->IncrementBy(1);

  // Check if we already have an executor for these arguments.
  std::shared_ptr<ExecutorsAndKeys> executors_and_keys;
  const int64 step_id = step_id_counter_.fetch_add(1);

  {
    tf_shared_lock l(callables_lock_);
    if (handle >= next_callable_handle_) {
      return errors::InvalidArgument("No such callable handle: ", handle);
    }
    executors_and_keys = callables_[handle].executors_and_keys;
  }

  if (!executors_and_keys) {
    return errors::InvalidArgument(
        "Attempted to run callable after handle was released: ", handle);
  }

  // NOTE(mrry): Debug options are not currently supported in the
  // callable interface.
  DebugOptions debug_options;
  RunStateArgs run_state_args(debug_options);

  // Configure a call frame for the step, which we use to feed and
  // fetch values to and from the executors.
  if (feed_tensors.size() != executors_and_keys->input_types.size()) {
    return errors::InvalidArgument(
        "Expected ", executors_and_keys->input_types.size(),
        " feed tensors, but got ", feed_tensors.size());
  }
  if (fetch_tensors != nullptr) {
    fetch_tensors->resize(executors_and_keys->output_types.size());
  } else if (!executors_and_keys->output_types.empty()) {
    return errors::InvalidArgument(
        "`fetch_tensors` must be provided when the callable has one or more "
        "outputs.");
  }

  size_t input_size = 0;
  for (auto& tensor : feed_tensors) {
    input_size += tensor.AllocatedBytes();
  }
  metrics::RecordGraphInputTensors(input_size);

  // A specialized CallFrame implementation that takes advantage of the
  // optimized RunCallable interface.

  RunCallableCallFrame call_frame(this, executors_and_keys.get(), &feed_tensors,
                                  fetch_tensors);

  if (LogMemory::IsEnabled()) {
    LogMemory::RecordStep(step_id, run_state_args.handle);
  }

  TF_RETURN_IF_ERROR(RunInternal(
      step_id, executors_and_keys->callable_options.run_options(), &call_frame,
      executors_and_keys.get(), run_metadata, threadpool_options));

  if (fetch_tensors != nullptr) {
    size_t output_size = 0;
    for (auto& tensor : *fetch_tensors) {
      output_size += tensor.AllocatedBytes();
    }
    metrics::RecordGraphOutputTensors(output_size);
  }

  return Status::OK();
}

::tensorflow::Status DirectSession::ReleaseCallable(CallableHandle handle) {
  mutex_lock l(callables_lock_);
  if (handle >= next_callable_handle_) {
    return errors::InvalidArgument("No such callable handle: ", handle);
  }
  callables_.erase(handle);
  return Status::OK();
}

DirectSession::Callable::~Callable() {
  // We must delete the fields in this order, because the destructor
  // of `executors_and_keys` will call into an object owned by
  // `function_info` (in particular, when deleting a kernel, it relies
  // on the `FunctionLibraryRuntime` to know if the kernel is stateful
  // or not).
  executors_and_keys.reset();
  function_info.reset();
}

}  // namespace tensorflow
