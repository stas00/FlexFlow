/* Copyright 2020 Stanford
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "simulator.h"
#include "model.h"
#include "queue"

int ParallelConfig::num_parts() const
{
  int nparts = 1;
  for (int i = 0; i < nDims; i++)
    nparts *= dim[i];
  return nparts;
}

bool ParallelConfig::is_data_parallel() const
{
  int nparts = 1;
  for (int i = 0; i < nDims; i++) {
    nparts *= dim[i];
    if ((i < nDims-1) && (dim[i] > 1))
      return false;
  }
  for (int i = 0; i < nparts; i++)
    if (device_ids[i] != i)
      return false;
  return true;
}

Device::Device(Device::DeviceType _type,
               int _node_id,
               int _gpu_id,
               size_t _capacity)
: node_id(_node_id),
  gpu_id(_gpu_id),
  bandwidth(0.0f),
  capacity(_capacity),
  type(_type)
{
  printf("Device: node_id(%d) gpu_id(%d) capacity(%zu)\n", node_id, gpu_id, capacity);
  assert(type == DEVICE_GPU);
}

Device::Device(Device::DeviceType _type,
               float _bandwidth)
: node_id(-1),
  gpu_id(-1),
  bandwidth(_bandwidth),
  capacity(0),
  type(_type)
{
  assert(type == DEVICE_COMM);
}

SimTask::SimTask()
{}

void SimTask::add_next_task(SimTask* task)
{
  next_tasks.push_back(task);
  task->counter ++;
}

std::string SimTask::get_type_str() const {
  switch (type) {
    case TASK_FORWARD:
      return "Forward";
    case TASK_BACKWARD:
      return "Backward";
    case TASK_COMM:
      return "Comm";
    case TASK_UPDATE:
      return "Update";
    case TASK_BARRIER:
      return "Barrier";
    default:
      assert(false && "Unknown task type");
  }
}

TaskManager::TaskManager(size_t _max_num_tasks)
: max_num_tasks(_max_num_tasks)
{
  tasks = (SimTask**) malloc(sizeof(SimTask*) * max_num_tasks);
  for (size_t i = 0; i < max_num_tasks; i++) {
    tasks[i] = new SimTask();
  }
}

void TaskManager::reset()
{
  global_task_id = 0;
  hash_to_forward_task.clear();
  hash_to_backward_task.clear();
}

SimTask* TaskManager::new_task()
{
  assert(global_task_id + 1 < max_num_tasks);
  SimTask* task = tasks[global_task_id++];
  task->ready_time = 0.0f;
  task->run_time = 0.0f;
  task->next_tasks.clear();
  task->counter = 0;
  task->device = NULL;
  task->op_name = NULL;
  return task;
}

SimTask* TaskManager::new_update_task()
{
  SimTask* task = new_task();
  task->type = SimTask::TASK_UPDATE;
  return task;
}

SimTask* TaskManager::new_barrier_task()
{
  SimTask* task = new_task();
  task->type = SimTask::TASK_BARRIER;
  return task;
}

SimTask* TaskManager::new_comm_task()
{
  SimTask* task = new_task();
  task->type = SimTask::TASK_COMM;
  return task;
}

SimTask* TaskManager::new_forward_task(Op* op, int idx)
{
  SimTask* task = new_task();
  task->type = SimTask::TASK_FORWARD;
  size_t hash = 17 * 31 + (size_t)(op);
  hash = hash * 31 + std::hash<int>()(idx);
  hash_to_forward_task[hash] = task;
  task->op_name = op->name;
  return task;
}

SimTask* TaskManager::new_backward_task(Op* op, int idx)
{
  SimTask* task = new_task();
  task->type = SimTask::TASK_BACKWARD;
  size_t hash = 17 * 31 + (size_t)(op);
  hash = hash * 31 + std::hash<int>()(idx);
  hash_to_backward_task[hash] = task;
  task->op_name = op->name;
  return task;
}

SimTask* TaskManager::get_forward_task(Op* op, int idx)
{
  size_t hash = 17 * 31 + (size_t)(op);
  hash = hash * 31 + std::hash<int>()(idx);
  assert(hash_to_forward_task.find(hash) != hash_to_forward_task.end());
  return hash_to_forward_task[hash];
}

SimTask* TaskManager::get_backward_task(Op* op, int idx)
{
  size_t hash = 17 * 31 + (size_t)(op);
  hash = hash * 31 + std::hash<int>()(idx);
  assert(hash_to_backward_task.find(hash) != hash_to_backward_task.end());
  return hash_to_backward_task[hash];
}

void Simulator::free_all()
{
  offset = 0;
}

void* Simulator::allocate(size_t num_elements, DataType type)
{
  size_t element_size = 0;
  switch (type) {
    case DT_FLOAT:
      element_size = sizeof(float);
      break;
    case DT_DOUBLE:
      element_size = sizeof(double);
      break;
    case DT_INT32:
      element_size = sizeof(int32_t);
      break;
    case DT_INT64:
      element_size = sizeof(int64_t);
      break;
    case DT_BOOLEAN:
      element_size = sizeof(bool);
      break;
    default:
      assert(false);
  }
  void* ret_ptr = base_ptr + offset;
  offset += element_size * num_elements;
  if ((size_t)offset > capacity) {
    fprintf(stderr, "Simulator cannot measure some operators' performance."
        " Increate --simulator-workspace-size to at least %zd\n", offset);
    exit(0);
  }
  return ret_ptr;
}

Device* Simulator::get_compute_device_by_id(int device_id)
{
  assert(id_to_compute_device.find(device_id) != id_to_compute_device.end());
  return id_to_compute_device[device_id];
}

Device* Simulator::get_inter_gpu_comm_device_by_ids(int src_id,
                                                    int dst_id)
{
  int hash = src_id * total_num_gpus + dst_id;
  assert(ids_to_inter_gpu_comm_device.find(hash) != ids_to_inter_gpu_comm_device.end());
  return ids_to_inter_gpu_comm_device[hash];
}

Device* Simulator::get_gpu_to_dram_comm_device_by_id(int gpu_id)
{
  assert(id_to_gputodram_comm_device.find(gpu_id) != id_to_gputodram_comm_device.end());
  return id_to_gputodram_comm_device[gpu_id];
}

Device* Simulator::get_dram_to_gpu_comm_device_by_id(int gpu_id)
{
  assert(id_to_dramtogpu_comm_device.find(gpu_id) != id_to_dramtogpu_comm_device.end());
  return id_to_dramtogpu_comm_device[gpu_id];
}

Device* Simulator::get_inter_node_comm_device_by_ids(int src_id,
                                                     int dst_id)
{
  int hash = src_id * total_num_gpus + dst_id;
  assert(ids_to_inter_node_comm_device.find(hash) != ids_to_inter_node_comm_device.end());
  return ids_to_inter_node_comm_device[hash];
}

void Simulator::add_task_dependencies_with_xfer(SimTask* src_task,
                                                SimTask* dst_task,
                                                size_t intersect)
{
  if (src_task->device == dst_task->device) {
    src_task->add_next_task(dst_task);
  } else if (src_task->device->node_id == dst_task->device->node_id) {
    // Intra-node communication
    SimTask* task = task_manager->new_comm_task();
    task->device = get_inter_gpu_comm_device_by_ids(src_task->device->gpu_id,
                                                    dst_task->device->gpu_id);
    task->run_time = (float)intersect * sizeof(float) / task->device->bandwidth;
    //printf("Comm task: run_time(%.4lf) size(%zu) bandwidth(%.4lf)\n",
    //       task->run_time, intersect * sizeof(float), task->device->bandwidth);
    src_task->add_next_task(task);
    task->add_next_task(dst_task);
  } else {
    // Inter-node communication
    SimTask* gpu_to_dram = task_manager->new_comm_task();
    gpu_to_dram->device = get_gpu_to_dram_comm_device_by_id(src_task->device->gpu_id);
    gpu_to_dram->run_time = (float)intersect * sizeof(float) / gpu_to_dram->device->bandwidth;
    SimTask* dram_to_dram = task_manager->new_comm_task();
    dram_to_dram->device = get_inter_node_comm_device_by_ids(src_task->device->node_id,
                                                             dst_task->device->node_id);
    dram_to_dram->run_time = (float)intersect * sizeof(float) / dram_to_dram->device->bandwidth;
    SimTask* dram_to_gpu = task_manager->new_comm_task();
    dram_to_gpu->device = get_dram_to_gpu_comm_device_by_id(dst_task->device->gpu_id);
    dram_to_gpu->run_time = (float)intersect * sizeof(float) / dram_to_gpu->device->bandwidth;
    src_task->add_next_task(gpu_to_dram);
    gpu_to_dram->add_next_task(dram_to_dram);
    dram_to_dram->add_next_task(dram_to_gpu);
    dram_to_gpu->add_next_task(dst_task);
  }
}

[[noreturn]] void handle_measure_operator_cost_unimplemented(Op const *op) {
    std::cerr << "measure_operator_cost not implemented for op "
              << op->name
              << " (type " << op->op_type << ")"
              << ". Please report this issue to the FlexFlow developers."
              << std::endl;
    std::abort();
}

CostMetrics Simulator::measure_operator_cost(Op* op, const ParallelConfig& config)
{
  size_t hash = 17 * 31 + (size_t)(op);
  hash = hash * 31 + std::hash<int>()(config.device_type);
  hash = hash * 31 + std::hash<int>()(config.nDims);
  for (int i = 0; i < config.nDims; i++)
    hash = hash * 31 + std::hash<int>()(config.dim[i]);
  std::map<size_t, CostMetrics>::const_iterator iter =
    hash_to_operator_cost.find(hash);
  if (iter == hash_to_operator_cost.end()) {
    CostMetrics cost_metrics;
    bool is_implemented = op->measure_operator_cost(this, config, cost_metrics);
    if (! is_implemented) {
      handle_measure_operator_cost_unimplemented(op);
    }
    hash_to_operator_cost[hash] = cost_metrics;
    return cost_metrics;
  } else {
    return iter->second;
  }
}

float Simulator::simulate_runtime(const FFModel* model,
                                  const std::map<Op*, ParallelConfig>& global,
                                  CompMode comp_mode)
{
  return this->simulate_runtime(model, global, comp_mode, "");
}

float Simulator::simulate_runtime(const FFModel* model,
                                  const std::map<Op*, ParallelConfig>& global,
                                  CompMode comp_mode,
                                  std::string const &export_file_name)
{
  task_manager->reset();
  // Step 1: register forward and backward tasks
  for (size_t l = 0; l < model->layers.size(); l++) {
    Op* op = model->layers[l];
    ParallelConfig config = global.find(op)->second;
    CostMetrics cost_metrics = measure_operator_cost(op, config);
    float forward_time = cost_metrics.forward_time;
    float backward_time = cost_metrics.backward_time;
    for (int j = 0; j < config.num_parts(); j++) {
      SimTask* task1 = task_manager->new_forward_task(op, j);
      task1->device = get_compute_device_by_id(config.device_ids[j]);
      task1->run_time = forward_time;
      if (comp_mode == COMP_MODE_TRAINING) {
        SimTask* task2 = task_manager->new_backward_task(op, j);
        task2->device = get_compute_device_by_id(config.device_ids[j]);
        task2->run_time = backward_time;
        task1->add_next_task(task2);
      }
    }
  }
  // Step 2: insert dependencies and comm. tasks before compute tasks
  for (size_t l = 0; l < model->layers.size(); l++) {
    Op* op = model->layers[l];
    ParallelConfig config = global.find(op)->second;
    for (int j = 0; j < op->numInputs; j++) {
      Tensor t = op->inputs[j];
      Op* pre_op = t.owner_op;
      if (pre_op == NULL)
        continue;
      ParallelConfig pre_config = global.find(pre_op)->second;
      for (int dstId = 0; dstId < config.num_parts(); dstId ++) {
        Domain dstR = op->get_input_tensor_shape(config, j, dstId);
        for (int srcId = 0; srcId < pre_config.num_parts(); srcId ++) {
          Domain srcR = pre_op->get_output_tensor_shape(pre_config, t.owner_idx, srcId);
          if (dstR.intersection(srcR).get_volume() > 0) {
            // Forward dependency
            {
              SimTask* dstT = task_manager->get_forward_task(op, dstId);
              SimTask* srcT = task_manager->get_forward_task(pre_op, srcId);
              add_task_dependencies_with_xfer(srcT, dstT, dstR.intersection(srcR).get_volume());
            }
            // Backward dependency
            if (comp_mode == COMP_MODE_TRAINING) {
              SimTask* dstT = task_manager->get_backward_task(op, dstId);
              SimTask* srcT = task_manager->get_backward_task(pre_op, srcId);
              add_task_dependencies_with_xfer(dstT, srcT, dstR.intersection(srcR).get_volume());
            }
          }
        }
      }
    }
  }
#ifdef FF_USE_NCCL
  // Do nothing since we will calculate NCCL cost at the end
#else
  // Step 2.5: add finals tasks for each compute device to capture the returning comm tasks
  // from parameter servers
  std::vector<SimTask*> finals;
  for (int d = 0; d < total_num_gpus; d++) {
    SimTask* t = task_manager->new_barrier_task();
    t->device = get_compute_device_by_id(d);
    t->run_time = 0;
    finals.push_back(t);
  }

  if (model->config.search_overlap_backward_update && comp_mode == COMP_MODE_TRAINING) {
    // Step 3a: consider backpropagation and weight update are overlapped
    for (int l = model->layers.size()-1; l >= 0; l--) {
      Op* op = model->layers[l];
      ParallelConfig pc = global.find(op)->second;
      for (int j = 0; j < op->numWeights; j++) {
        std::set<int> synched;
        for (int firstId = 0; firstId < pc.num_parts(); firstId++)
          if (synched.find(firstId) == synched.end()) {
            synched.insert(firstId);
            Domain firstR = op->get_weight_tensor_shape(pc, j, firstId);
            // Add a compute task for parameter update
            SimTask* updateT = task_manager->new_update_task();
            updateT->device = get_compute_device_by_id(pc.device_ids[firstId]);
            // TODO add parameter synchronization time
            updateT->run_time = 0.0f; // Assume update task takes no time
            for (int nextId = firstId+1; nextId < pc.num_parts(); nextId++) {
              Domain nextR = op->get_weight_tensor_shape(pc, j, nextId);
              if (firstR.intersection(nextR).get_volume() > 0) {
                // Assert all or nothing:
                // The two weights must be fully overlapped or not at all
                assert(firstR == nextR);
                assert(synched.find(nextId) == synched.end());
                synched.insert(nextId);
                // Add comm. tasks from backT to updateT
                SimTask* backT = task_manager->get_backward_task(op, nextId);
                add_task_dependencies_with_xfer(backT, updateT, firstR.get_volume());
                // Add comm. tasks from updateT to finalT
                SimTask* finalT = finals[backT->device->gpu_id];
                add_task_dependencies_with_xfer(updateT, finalT, firstR.get_volume());
              }
            }
          }
      }
    }
  } else if (comp_mode == COMP_MODE_TRAINING) {
    // Step 3b: Bulk Synchronous Model
    // Add a per-device barrier before weight update
    std::vector<SimTask*> barriers;
    for (int d = 0; d < total_num_gpus; d++) {
      SimTask* t = task_manager->new_barrier_task();
      t->device = get_compute_device_by_id(d);
      t->run_time = 0;
      barriers.push_back(t);
    }
    for (size_t l = 0; l < model->layers.size(); l++) {
      Op* op = model->layers[l];
      ParallelConfig pc = global.find(op)->second;
      for (int j = 0; j < pc.num_parts(); j++) {
        SimTask* backT = task_manager->get_backward_task(op, j);
        backT->add_next_task(barriers[backT->device->gpu_id]);
      }
    }
    for (size_t l = 0; l < model->layers.size(); l++) {
      Op* op = model->layers[l];
      ParallelConfig pc = global.find(op)->second;
      for (int j = 0; j < op->numWeights; j++) {
        std::set<int> synched;
        for (int firstId = 0; firstId < pc.num_parts(); firstId++)
          if (synched.find(firstId) == synched.end()) {
            synched.insert(firstId);
            Domain firstR = op->get_weight_tensor_shape(pc, j, firstId);
            // Add a compute task for parameter update
            SimTask* updateT = task_manager->new_update_task();
            updateT->device = get_compute_device_by_id(pc.device_ids[firstId]);
            updateT->run_time = 0.0f; // Assume update task takes no time
            barriers[updateT->device->gpu_id]->add_next_task(updateT);
            for (int nextId = firstId+1; nextId < pc.num_parts(); nextId++) {
              Domain nextR = op->get_weight_tensor_shape(pc, j, nextId);
              if (firstR.intersection(nextR).get_volume() > 0) {
                // Assert all or nothing:
                // The two weights must be fully overlapped or not at all
                assert(firstR == nextR);
                assert(synched.find(nextId) == synched.end());
                synched.insert(nextId);
                SimTask* backT = task_manager->get_backward_task(op, nextId);
                assert(backT->device->gpu_id == pc.device_ids[nextId]);
                SimTask* barrierT = barriers[backT->device->gpu_id];
                // Add comm. tasks from barrierT to updateT
                add_task_dependencies_with_xfer(barrierT, updateT, firstR.get_volume());
                // Add comm. tasks from updateT to finalT
                SimTask* finalT = finals[backT->device->gpu_id];
                add_task_dependencies_with_xfer(updateT, finalT, firstR.get_volume());
              }
            }
          }
      }
    }
  } else {
    assert(comp_mode == COMP_MODE_INFERENCE);
  }
#endif
  // Step 4: add ready tasks into ready_queue
  std::priority_queue<SimTask*, std::vector<SimTask*>, SimTaskCompare> ready_queue;
  for (size_t i = 0; i < task_manager->global_task_id; i++)
    if (task_manager->tasks[i]->counter == 0)
      ready_queue.push(task_manager->tasks[i]);
  // Step 5: perform simulation
  float sim_time = 0.0f;
  std::map<Device*, float> device_times;
  size_t idx = 0;
  DotFile<SimTask *> taskGraph;
  bool export_taskgraph = (export_file_name != "");
  if (export_taskgraph) {
    taskGraph.set_filename(export_file_name);
  }
  while (!ready_queue.empty()) {
    // Find the task with the earliest start time
    SimTask* t = ready_queue.top();
    ready_queue.pop();
    float ready_time = 0;
    if (device_times.find(t->device) != device_times.end()) {
      ready_time = device_times[t->device];
    }
    float start_time = std::max(ready_time, t->ready_time);
    float end_time = start_time + t->run_time;
    device_times[t->device] = end_time;
    if (export_taskgraph) {
      std::map<std::string, std::string> nodeAttrs;
      std::ostringstream label;
      label << "\"{ ";
      if (t->op_name != NULL) {
        label << t->op_name << " | ";
      }
      label << t->get_type_str() << " | ";
      label << "{ " << start_time << " | " << end_time << " }";
      label << " }\"";
      nodeAttrs["label"] = label.str();
      nodeAttrs["shape"] = "record";
      taskGraph.add_node(t, nodeAttrs);
    }
    /* printf("task[%d] type(%d) run_time(%.4lf) ready_time(%.4lf) start_time(%.4lf) device(%d)\n", */
    /*     idx, t->type, t->run_time, ready_time, start_time, t->device->gpu_id); */
    if (end_time > sim_time)
      sim_time = end_time;
    for (size_t i = 0; i < t->next_tasks.size(); i++) {
      SimTask* next = t->next_tasks[i];
      if (export_taskgraph) {
        taskGraph.add_edge(t, next);
      }
      next->ready_time = std::max(next->ready_time, end_time);
      next->counter --;
      if (next->counter == 0) {
        ready_queue.push(next);
      }
    }
    idx++;
  }
  if (export_taskgraph) {
    taskGraph.close();
  }
  // Assert all tasks were processed
  assert(idx == task_manager->global_task_id);
#ifdef FF_USE_NCCL
  if (comp_mode == COMP_MODE_TRAINING) {
    for (size_t l = 0; l < model->layers.size(); l++) {
      Op* op = model->layers[l];
      ParallelConfig pc = global.find(op)->second;
      // Since all NCCL calls are blocking, we can add the NCCL cost
      // sequentially 
      for (int j = 0; j < op->numWeights; j++) {
        std::set<int> synched;
        for (int firstId = 0; firstId < pc.num_parts(); firstId++)
          if (synched.find(firstId) == synched.end()) {
            synched.insert(firstId);
            Domain firstR = op->get_weight_tensor_shape(pc, j, firstId);
            Device* firstDevice = get_compute_device_by_id(pc.device_ids[firstId]);
            float nccl_time = 0.0f;
            for (int nextId = firstId+1; nextId < pc.num_parts(); nextId++) {
              Domain nextR = op->get_weight_tensor_shape(pc, j, nextId);
              if (firstR.intersection(nextR).get_volume() > 0) {
                // Assert all or nothing:
                // The two weights must be fully overlapped or not at all
                assert(firstR == nextR);
                assert(synched.find(nextId) == synched.end());
                synched.insert(nextId);
                Device* nextDevice = get_compute_device_by_id(pc.device_ids[nextId]);
                // Compute the bandwidth between firstDevice/nextDevice
                float bandwidth = 0.0f;
                if (firstDevice->node_id == nextDevice->node_id) {
                  Device* commDevice = get_inter_gpu_comm_device_by_ids(
                      firstDevice->gpu_id, nextDevice->gpu_id);
                  bandwidth = commDevice->bandwidth;
                } else {
                  Device* commDevice = get_inter_node_comm_device_by_ids(
                      firstDevice->node_id, nextDevice->node_id);
                  bandwidth = commDevice->bandwidth;
                }
                nccl_time = std::max(nccl_time, (float)firstR.get_volume() * sizeof(float) / bandwidth);
              }
            }
            // Add ncclTime to sim_time given nccl calls are blocking
            sim_time += nccl_time;
          }
      }
    }
  } else {
    assert(comp_mode == COMP_MODE_INFERENCE);
  }
#endif
  // Step 6: add penalty to strategies that exceed the memory limits on devices
  std::vector<size_t> gpu_mem_usage(total_num_gpus, 0);
  float memory_penalty = 0.0f;
  for (size_t l = 0; l < model->layers.size(); l++) {
    Op* op = model->layers[l];
    ParallelConfig config = global.find(op)->second;
    CostMetrics cost_metrics = measure_operator_cost(op, config);
    size_t memory_requirement = cost_metrics.memory_requirement;
    for (int j = 0; j < config.num_parts(); j++) {
      gpu_mem_usage[config.device_ids[j]] += memory_requirement;
    }
  }
  if (export_file_name != "") {  
    for (int i = 0; i < total_num_gpus; i++) {
        printf("Before penalty, dev id %d, usage %zu \n", i, gpu_mem_usage[i]); 
    }
  }
  // Penalize the total runtiem by 1ms if we exceed the memory budget by 1MB
  for (int i = 0; i < total_num_gpus; i++) {
    Device* gpu = get_compute_device_by_id(i);
    if (gpu_mem_usage[i] > gpu->capacity)
      memory_penalty += (gpu_mem_usage[i] - gpu->capacity) * 1e-6;
  }
  //if (memory_penalty > 0.0f)
  //  printf("Memory penalty = %.4lf ms\n", memory_penalty);
  return sim_time + memory_penalty;
}
