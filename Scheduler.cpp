//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//
//  P-Mapper Algorithm
//

#include <cstdint>
#include "Scheduler.hpp"

#include <algorithm>
#include <map>
#include <set>

#include <queue>
#include <vector>
#include <climits>


vector<MachineId_t> machine_order;
map<TaskId_t, VMId_t> task_to_vm;
set<VMId_t> migrating_vms;
set<TaskId_t> pending_set;
queue<TaskId_t> pending_tasks;
static set<MachineId_t> changing_machines;

bool compareMachines(MachineId_t a, MachineId_t b) {
    MachineInfo_t first = Machine_GetInfo(a);
    MachineInfo_t second = Machine_GetInfo(b);

    double power_first, power_second;
    
    if (first.s_states.empty()) {
        power_first = 0;
    } else {
        power_first = first.s_states[0];
    }
    if (second.s_states.empty()) {
        power_second = 0;
    } else {
        power_second = second.s_states[0];
    }

    if (first.p_states.empty()) {
        power_first += 0;
    } else {
        power_first += double(first.num_cpus) * double(first.p_states[0]);
    }
    if (second.p_states.empty()) {
        power_second += 0;
    } else {
        power_second += double(second.num_cpus) * double(second.p_states[0]);
    }

    return power_first < power_second;
}

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Initializing scheduler", 3);
    machine_order.clear();
    machines.clear();

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineInfo_t info = Machine_GetInfo(i);
        machines.push_back(info.machine_id);
        machine_order.push_back(info.machine_id);
    }

    sort(machine_order.begin(), machine_order.end(), compareMachines);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    if (task_to_vm.find(task_id) != task_to_vm.end()) {
        return;
    }
    TaskInfo_t task = GetTaskInfo(task_id);
    Priority_t priority = LOW_PRIORITY;
    if (task.required_sla == SLA0 || task.required_sla == SLA1) {
        priority = HIGH_PRIORITY;
    } else if (task.required_sla == SLA2) {
        priority = MID_PRIORITY;
    }

    VMId_t best_vm = 0;
    double best_score = 0.0;
    bool found = false;

    for (unsigned i = 0; i < vms.size(); i++) {
        VMId_t vm_id = vms[i];
        if (migrating_vms.find(vm_id) == migrating_vms.end()) {
            VMInfo_t vm_info = VM_GetInfo(vm_id);
            if (vm_info.cpu == task.required_cpu && vm_info.vm_type == task.required_vm) {
                MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
                if (machine_info.s_state == S0 && machine_info.cpu == task.required_cpu && (!task.gpu_capable || machine_info.gpus) && machine_info.memory_used + task.required_memory <= machine_info.memory_size) {
                    unsigned cpu_headroom = 0;
                    if (machine_info.num_cpus > machine_info.active_tasks) {
                        cpu_headroom = machine_info.num_cpus - machine_info.active_tasks;
                    }

                    bool sla = false;
                    if (task.required_sla == SLA0 && machine_info.active_tasks + 1 < machine_info.num_cpus) {
                        sla = true;
                    } else if (task.required_sla == SLA1 && (machine_info.active_tasks < machine_info.num_cpus || cpu_headroom >= 1)) {
                        sla = true;
                    } else if (machine_info.active_tasks <= machine_info.num_cpus) {
                        sla = true;
                    }

                    if (sla) {
                        double cpu_utilization = double(machine_info.active_tasks) / double(machine_info.num_cpus);
                        double memory_utilization = double(machine_info.memory_used) / double(machine_info.memory_size);
                        double utilization = max(cpu_utilization, memory_utilization);
                        bool util_ok = false;

                        if (task.required_sla == SLA0 && utilization <= 0.60) {
                            util_ok = true;
                        } else if (task.required_sla == SLA1 && utilization <= 0.95) {
                            util_ok = true;
                        } else {
                            util_ok = true;
                        }

                        if (util_ok) {
                            double power = 0;
                            if (!machine_info.s_states.empty()) {
                                power = machine_info.s_states[0];
                            }

                            if (!machine_info.p_states.empty()) {
                                power += double(machine_info.num_cpus) * double(machine_info.p_states[0]);
                            }

                            double score = power + utilization * 1000.0 + vm_info.active_tasks.size() * 25.0;

                            if (!found || score < best_score) {
                                found = true;
                                best_score = score;
                                best_vm = vm_id;
                            }
                        }
                    }
                }
            }
        }
    }

    if (found) {
        VM_AddTask(best_vm, task_id, priority);
        task_to_vm[task_id] = best_vm;
        return;
    }

    MachineId_t best_machine = 0;
    best_score = 0.0;
    found = false;

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_order[i]);
        if (machine_info.s_state == S0 && machine_info.cpu == task.required_cpu && (!task.gpu_capable || machine_info.gpus) && 
            machine_info.memory_used + task.required_memory + VM_MEMORY_OVERHEAD <= machine_info.memory_size) {
                
            bool sla = false;
            if ((task.required_sla == SLA0 || task.required_sla == SLA1) && machine_info.active_tasks < machine_info.num_cpus) {
                sla = true;
            } else if (machine_info.active_tasks <= machine_info.num_cpus) {
                sla = true;
            }

            if (sla) {
                double cpu_utilization = double(machine_info.active_tasks) / double(machine_info.num_cpus);
                double memory_utilization = double(machine_info.memory_used) / double(machine_info.memory_size);
                double utilization = max(cpu_utilization, memory_utilization);
                bool util_ok = false;

                if (task.required_sla == SLA0 && utilization <= 0.75) {
                    util_ok = true;
                } else if (task.required_sla == SLA1 && utilization <= 0.90) {
                    util_ok = true;
                } else {
                    util_ok = true;
                }

                if (util_ok) {
                    double power = 0;
                    if (!machine_info.s_states.empty()) {
                        power = machine_info.s_states[0];
                    }

                    if (!machine_info.p_states.empty()) {
                        power += double(machine_info.num_cpus) * double(machine_info.p_states[0]);
                    }

                    double score = power + utilization * 1000.0 + machine_info.active_vms * 20.0;

                    if (!found || score < best_score) {
                        found = true;
                        best_score = score;
                        best_machine = machine_order[i];
                    }
                }
            }
        }
    }

    if (found) {
        MachineInfo_t machine_info = Machine_GetInfo(best_machine);
        if (machine_info.s_state == S0) {
            VMId_t vm_id = VM_Create(task.required_vm, task.required_cpu);
            VM_Attach(vm_id, best_machine);
            VM_AddTask(vm_id, task_id, priority);
            task_to_vm[task_id] = vm_id;
            vms.push_back(vm_id);
            return;
        }
    }

    if (pending_set.find(task_id) == pending_set.end()) {
        pending_tasks.push(task_id);
        pending_set.insert(task_id);
    }

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_order[i]);
        if (machine_info.cpu == task.required_cpu && (!task.gpu_capable || machine_info.gpus) && 
            machine_info.memory_used + task.required_memory + VM_MEMORY_OVERHEAD <= machine_info.memory_size && 
            machine_info.s_state != S0 && changing_machines.find(machine_order[i]) == changing_machines.end()) {

            Machine_SetState(machine_order[i], S0);
            changing_machines.insert(machine_order[i]);
            return;
        }
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    int pending_size = pending_tasks.size();
    while (pending_size > 0 && !pending_tasks.empty()) {
        pending_size--;
        TaskId_t task_id = pending_tasks.front();
        pending_tasks.pop();
        pending_set.erase(task_id);

        if (!IsTaskCompleted(task_id)) {
            if (task_to_vm.find(task_id) == task_to_vm.end()) {
                NewTask(now, task_id);
            }
        }
    }

}

void Scheduler::Shutdown(Time_t time) {
for (unsigned i = 0; i < vms.size(); i++) {
    if (migrating_vms.find(vms[i]) == migrating_vms.end()) {
        VMInfo_t vm_info = VM_GetInfo(vms[i]);
        if (vm_info.active_tasks.empty()) {
            VM_Shutdown(vms[i]);
        }
    }
}

    vms.clear();
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    if (task_to_vm.find(task_id) == task_to_vm.end()) {
        return;
    }

    VMId_t vm_id = task_to_vm[task_id];
    task_to_vm.erase(task_id);

    if (migrating_vms.find(vm_id) != migrating_vms.end()) {
        return;
    }

    auto it = find(vms.begin(), vms.end(), vm_id);
    if (it == vms.end()) {
        return;
    }

    VMInfo_t vm = VM_GetInfo(vm_id);
    if (vm.active_tasks.empty()) {
        vms.erase(it);
        VM_Shutdown(vm_id);
    } else {
        vector<MachineId_t> active_machines;

        for (unsigned i = 0; i < machine_order.size(); i++) {
            MachineInfo_t info = Machine_GetInfo(machine_order[i]);
            if (info.s_state == S0 && info.active_tasks > 0) {
                active_machines.push_back(machine_order[i]);
            }
        }

        if (active_machines.size() >= 2) {
            sort(active_machines.begin(), active_machines.end(), [](MachineId_t a, MachineId_t b) {
                return Machine_GetInfo(a).active_tasks < Machine_GetInfo(b).active_tasks;
            });

            MachineId_t source_machine = active_machines[0];
            MachineInfo_t source_machine_info = Machine_GetInfo(source_machine);

            double source_utilization = 0.0;
            if (source_machine_info.num_cpus != 0) {
                source_utilization = double(source_machine_info.active_tasks) / double(source_machine_info.num_cpus);
            }

            if (source_utilization <= 0.50) {
                VMId_t source_vm = VMId_t(UINT_MAX);
                unsigned smallest_vm_load = UINT_MAX;

                for (unsigned i = 0; i < vms.size(); i++) {
                    VMId_t candidate_vm = vms[i];

                    if (migrating_vms.find(candidate_vm) == migrating_vms.end()) {
                        VMInfo_t candidate_info = VM_GetInfo(candidate_vm);

                        if (candidate_info.machine_id == source_machine && !candidate_info.active_tasks.empty()) {
                            bool strict_task_found = false;

                            for (unsigned j = 0; j < candidate_info.active_tasks.size(); j++) {
                                SLAType_t sla = GetTaskInfo(candidate_info.active_tasks[j]).required_sla;
                                if (sla == SLA0 || sla == SLA1) {
                                    strict_task_found = true;
                                    break;
                                }
                            }

                            if (!strict_task_found &&candidate_info.active_tasks.size() < smallest_vm_load) {
                                smallest_vm_load = candidate_info.active_tasks.size();
                                source_vm = candidate_vm;
                            }
                        }
                    }
                }

                if (source_vm != VMId_t(UINT_MAX)) {
                    VMInfo_t source_vm_info = VM_GetInfo(source_vm);
                    unsigned vm_memory = VM_MEMORY_OVERHEAD;

                    for (unsigned i = 0; i < source_vm_info.active_tasks.size(); i++) {
                        vm_memory += GetTaskMemory(source_vm_info.active_tasks[i]);
                    }

                    for (int i = int(active_machines.size()) - 1; i >= 0; i--) {
                        MachineId_t target_machine = active_machines[i];

                        if (target_machine != source_machine && changing_machines.find(target_machine) == changing_machines.end()) {
                            MachineInfo_t target_info = Machine_GetInfo(target_machine);

                            if (target_info.cpu == source_vm_info.cpu && target_info.s_state == S0) {
                                double target_utilization = 1.0;
                                if (target_info.num_cpus != 0) {
                                    target_utilization = double(target_info.active_tasks + source_vm_info.active_tasks.size()) / double(target_info.num_cpus);
                                }

                                if (target_utilization <= 0.70) {
                                    bool target_has_strict = false;

                                    for (unsigned j = 0; j < vms.size(); j++) {
                                        VMId_t target_vm = vms[j];

                                        if (migrating_vms.find(target_vm) == migrating_vms.end()) {
                                            VMInfo_t target_vm_info = VM_GetInfo(target_vm);

                                            if (target_vm_info.machine_id == target_machine) {
                                                for (unsigned k = 0; k < target_vm_info.active_tasks.size(); k++) {
                                                    SLAType_t sla = GetTaskInfo(target_vm_info.active_tasks[k]).required_sla;
                                                    if (sla == SLA0 || sla == SLA1) {
                                                        target_has_strict = true;
                                                        break;
                                                    }
                                                }
                                            }
                                        }

                                        if (target_has_strict) {
                                            break;
                                        }
                                    }

                                    if (!target_has_strict && target_info.memory_used + vm_memory <= target_info.memory_size && 
                                        target_info.active_tasks + source_vm_info.active_tasks.size() <= target_info.num_cpus) {

                                        migrating_vms.insert(source_vm);
                                        VM_Migrate(source_vm, target_machine);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}



// Public interface below

static Scheduler pmapper_scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    pmapper_scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    pmapper_scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    pmapper_scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);

    for (MachineId_t candidate : machine_order) {
        MachineInfo_t m = Machine_GetInfo(candidate);

        if (m.cpu == info.cpu) {
            if (m.s_state != S0) {
                if (changing_machines.find(candidate) == changing_machines.end()) {
                    Machine_SetState(candidate, S0);
                    changing_machines.insert(candidate);
                    return;
                }
            }
        }
    }
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    pmapper_scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    pmapper_scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    
    pmapper_scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);

    auto it = task_to_vm.find(task_id);

    if (it != task_to_vm.end()) {
        VMId_t vm_id = it->second;
        VMInfo_t vm = VM_GetInfo(vm_id);
        MachineInfo_t cur = Machine_GetInfo(vm.machine_id);

        if (!(cur.active_tasks < cur.num_cpus && cur.memory_used < cur.memory_size)) {
            unsigned vm_mem = VM_MEMORY_OVERHEAD;
            for (TaskId_t t : vm.active_tasks) {
                vm_mem += GetTaskMemory(t);
            }

            for (MachineId_t machine_id : machine_order) {
                MachineInfo_t machine_info = Machine_GetInfo(machine_id);

                if (machine_info.machine_id != vm.machine_id) {
                    if (machine_info.cpu == vm.cpu && machine_info.s_state == S0) {
                        if (changing_machines.find(machine_id) == changing_machines.end()) {
                            if (machine_info.memory_used + vm_mem <= machine_info.memory_size) {
                                if (machine_info.active_tasks < machine_info.num_cpus) {
                                    if (migrating_vms.find(vm_id) == migrating_vms.end()) {
                                        migrating_vms.insert(vm_id);
                                        VM_Migrate(vm_id, machine_id);
                                    }
                                    return;
                                }
                            }
                        }
                    }
                }
            }

            for (MachineId_t machine_id : machine_order) {
                MachineInfo_t machine_info = Machine_GetInfo(machine_id);

                if (machine_info.cpu == vm.cpu) {
                    if (machine_info.s_state != S0) {
                        if (changing_machines.find(machine_id) == changing_machines.end()) {
                            Machine_SetState(machine_id, S0);
                            changing_machines.insert(machine_id);
                            return;
                        }
                    }
                }
            }
        }
    }
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    changing_machines.erase(machine_id);
}
