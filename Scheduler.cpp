//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//  Load Balancing Algorithm
//

#include <cstdint>
#include "Scheduler.hpp"

#include <algorithm>
#include <climits>
#include <map>
#include <vector>

struct MachineData {
    MachineId_t machine_id;
    CPUType_t cpu_type;
    bool gpu;
    unsigned memory_size;
    unsigned num_cpus;
};


vector<MachineData> machine_data;
map<MachineId_t, vector<VMId_t>> machine_to_vms;
map<VMId_t, MachineId_t> vm_to_machine;
map<TaskId_t, VMId_t> task_to_vm;

void Scheduler::Init() {
    unsigned total = Machine_GetTotal();
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(total), 3);
    SimOutput("Scheduler::Init(): Initializing SLA-first load balancer", 1);

    for (unsigned i = 0; i < total; i++) {
        MachineInfo_t info = Machine_GetInfo(i);

        machines.push_back(info.machine_id);
        machine_data.push_back({info.machine_id, info.cpu, info.gpus, info.memory_size, info.num_cpus});

        if (info.s_state != S0) {
            Machine_SetState(info.machine_id, S0);
        }

        for (unsigned core = 0; core < info.num_cpus; core++) {
            Machine_SetCorePerformance(info.machine_id, core, P0);
        }
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t task_info = GetTaskInfo(task_id);
    Priority_t priority = LOW_PRIORITY;
    if (task_info.required_sla == SLA0 || task_info.required_sla == SLA1) {
        priority = HIGH_PRIORITY;
    } else if (task_info.required_sla == SLA2) {
        priority = MID_PRIORITY;
    }

    MachineId_t best_machine = MachineId_t(UINT_MAX);
    VMId_t best_vm = VMId_t(UINT_MAX);
    bool best_needs_new_vm = false;
    unsigned best_strict_load = UINT_MAX;
    unsigned best_total_load = UINT_MAX;
    unsigned best_memory_used = UINT_MAX;

    for (unsigned i = 0; i < machine_data.size(); i++) {
        MachineInfo_t info = Machine_GetInfo(machine_data[i].machine_id);

        if (info.s_state == S0 && info.cpu == task_info.required_cpu &&
            (!task_info.gpu_capable || info.gpus)) {
            bool vm_ok = true;
            if (task_info.required_vm == AIX && info.cpu != POWER) {
                vm_ok = false;
            }
            if (task_info.required_vm == WIN && info.cpu != ARM && info.cpu != X86) {
                vm_ok = false;
            }

            if (vm_ok) {
                VMId_t matching_vm = VMId_t(UINT_MAX);
                auto machine_it = machine_to_vms.find(machine_data[i].machine_id);
                if (machine_it != machine_to_vms.end()) {
                    for (unsigned j = 0; j < machine_it->second.size(); j++) {
                        VMInfo_t vm_info = VM_GetInfo(machine_it->second[j]);
                        if (vm_info.cpu == task_info.required_cpu && vm_info.vm_type == task_info.required_vm) {
                            matching_vm = machine_it->second[j];
                            break;
                        }
                    }
                }

                bool needs_new_vm = matching_vm == VMId_t(UINT_MAX);
                unsigned extra_overhead;
                if (needs_new_vm) {
                    extra_overhead = VM_MEMORY_OVERHEAD;
                } else {
                    extra_overhead = 0;
                }

                if (info.memory_used + task_info.required_memory + extra_overhead <= info.memory_size) {
                    unsigned strict_load = 0;
                    if (machine_it != machine_to_vms.end()) {
                        for (unsigned j = 0; j < machine_it->second.size(); j++) {
                            VMInfo_t vm_info = VM_GetInfo(machine_it->second[j]);
                            for (unsigned k = 0; k < vm_info.active_tasks.size(); k++) {
                                SLAType_t sla = GetTaskInfo(vm_info.active_tasks[k]).required_sla;
                                if (sla == SLA0 || sla == SLA1) {
                                    strict_load++;
                                }
                            }
                        }
                    }

                    unsigned total_load = info.active_tasks;
                    unsigned memory_used = info.memory_used;

                    bool better = false;
                    if (best_machine == MachineId_t(UINT_MAX)) {
                        better = true;
                    } else if (task_info.required_sla == SLA0 || task_info.required_sla == SLA1) {
                        if (strict_load < best_strict_load) {
                            better = true;
                        } else if (strict_load == best_strict_load && total_load < best_total_load) {
                            better = true;
                        } else if (strict_load == best_strict_load && total_load == best_total_load && memory_used < best_memory_used) {
                            better = true;
                        }
                    } else {
                        if (total_load < best_total_load) {
                            better = true;
                        } else if (total_load == best_total_load && memory_used < best_memory_used) {
                            better = true;
                        }
                    }

                    if (better) {
                        best_machine = machine_data[i].machine_id;
                        best_vm = matching_vm;
                        best_needs_new_vm = needs_new_vm;
                        best_strict_load = strict_load;
                        best_total_load = total_load;
                        best_memory_used = memory_used;
                    }
                }
            }
        }
    }

    if (best_machine == MachineId_t(UINT_MAX)) {
        SimOutput("Scheduler::NewTask(): No compatible host found for task " + to_string(task_id), 0);
        return;
    }

    if (best_needs_new_vm) {
        best_vm = VM_Create(task_info.required_vm, task_info.required_cpu);
        VM_Attach(best_vm, best_machine);
        machine_to_vms[best_machine].push_back(best_vm);
        vm_to_machine[best_vm] = best_machine;
        vms.push_back(best_vm);
    }

    VM_AddTask(best_vm, task_id, priority);
    task_to_vm[task_id] = best_vm;

    MachineInfo_t best_info = Machine_GetInfo(best_machine);
    for (unsigned core = 0; core < best_info.num_cpus; core++) {
        Machine_SetCorePerformance(best_machine, core, P0);
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    for (unsigned i = 0; i < machine_data.size(); i++) {
        MachineInfo_t info = Machine_GetInfo(machine_data[i].machine_id);

        if (info.s_state == S0) {
            CPUPerformance_t p_state = P0;
            if (info.active_tasks == 0) {
                p_state = P1;
            }

            for (unsigned core = 0; core < info.num_cpus; core++) {
                Machine_SetCorePerformance(info.machine_id, core, p_state);
            }
        }
    }
}

void Scheduler::Shutdown(Time_t time) {
    for (unsigned i = 0; i < vms.size(); i++) {
        VMInfo_t vm_info = VM_GetInfo(vms[i]);
        if (vm_info.active_tasks.empty()) {
            VM_Shutdown(vms[i]);
        }
    }

    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    auto it = task_to_vm.find(task_id);
    if (it != task_to_vm.end()) {
        MachineId_t machine_id = vm_to_machine[it->second];
        task_to_vm.erase(it);

        MachineInfo_t info = Machine_GetInfo(machine_id);
        if (info.active_tasks <= 1) {
            for (unsigned core = 0; core < info.num_cpus; core++) {
                Machine_SetCorePerformance(machine_id, core, P1);
            }
        }
    }

    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);
}

static Scheduler load_scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    load_scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) +
              " at time " + to_string(time), 4);
    load_scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) +
              " completed at time " + to_string(time), 4);
    load_scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) +
              " was detected at time " + to_string(time), 0);

    MachineInfo_t info = Machine_GetInfo(machine_id);
    for (unsigned core = 0; core < info.num_cpus; core++) {
        Machine_SetCorePerformance(machine_id, core, P0);
    }
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) +
              " was completed at time " + to_string(time), 4);
    load_scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    load_scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time) / 1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);

    load_scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);

    auto it = task_to_vm.find(task_id);
    if (it != task_to_vm.end()) {
        MachineId_t machine_id = vm_to_machine[it->second];
        MachineInfo_t info = Machine_GetInfo(machine_id);
        for (unsigned core = 0; core < info.num_cpus; core++) {
            Machine_SetCorePerformance(machine_id, core, P0);
        }
    }
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
}