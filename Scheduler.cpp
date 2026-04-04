//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//
//  E-Eco Algorithm
//


#include <cstdint>
#include "Scheduler.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <vector>
#include <climits>

struct MachineData {
    MachineId_t machine_id;
    unsigned memory_size;
    unsigned num_cpus;
    bool gpu;

    CPUType_t cpu_type;
    MachineState_t s_state;

    bool state_changing;
    MachineState_t target_state;

    unsigned s0_power;
    unsigned p0_power;
    unsigned perf0;
};

vector<MachineData> machine_data;
vector<MachineId_t> machine_order;
map<MachineId_t, vector<VMId_t> > machine_to_vms;
map<VMId_t, MachineId_t> vm_to_machine;
map<TaskId_t, VMId_t> task_to_vm;
map<MachineId_t, Time_t> idle_since;
queue<TaskId_t> pending_tasks;
set<TaskId_t> pending_set;
set<VMId_t> migrating_vms;

Time_t SECOND = 1000000;
Time_t IDLE_TO_S2 = 10 * SECOND;
Time_t S2_TO_S5 = 30 * SECOND;

bool compareMachines(MachineId_t a, MachineId_t b) {
    MachineInfo_t first = Machine_GetInfo(a);
    MachineInfo_t second = Machine_GetInfo(b);

    double power_first = 0.0;
    double power_second = 0.0;

    if (!first.s_states.empty()) {
        power_first += first.s_states[0];
    }
    if (!second.s_states.empty()) {
        power_second += second.s_states[0];
    }

    if (!first.p_states.empty()) {
        power_first += double(first.num_cpus) * double(first.p_states[0]);
    }
    if (!second.p_states.empty()) {
        power_second += double(second.num_cpus) * double(second.p_states[0]);
    }

    return power_first < power_second;
}

int findMachineIndex(MachineId_t machine_id) {
    for (unsigned i = 0; i < machine_data.size(); i++) {
        if (machine_data[i].machine_id == machine_id) {
            return int(i);
        }
    }
    return -1;
}

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Initializing E-Eco scheduler", 3);

    machine_data.resize(Machine_GetTotal());

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineData data;
        MachineInfo_t info = Machine_GetInfo(i);
        data.machine_id = info.machine_id;
        data.memory_size = info.memory_size;
        data.num_cpus = info.num_cpus;
        data.gpu = info.gpus;
        data.cpu_type = info.cpu;
        data.s_state = info.s_state;
        data.state_changing = false;
        data.target_state = S0;

        if (info.s_states.empty()) {
            data.s0_power = 0;
        } else {
            data.s0_power = info.s_states[0];
        }

        if (info.p_states.empty()) {
            data.p0_power = 0;
        } else {
            data.p0_power = info.p_states[0];
        }

        if (info.performance.empty()) {
            data.perf0 = 0;
        } else {
            data.perf0 = info.performance[0];
        }

        machines.push_back(info.machine_id);
        machine_order.push_back(info.machine_id);
        machine_data[i] = data;

        if (info.s_state != S0) {
            Machine_SetState(info.machine_id, S0);
            machine_data[i].state_changing = true;
            machine_data[i].target_state = S0;
        }
    }

    sort(machine_order.begin(), machine_order.end(), compareMachines);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    if (IsTaskCompleted(task_id)) {
        return;
    }
    if (task_to_vm.find(task_id) != task_to_vm.end()) {
        return;
    }

    TaskInfo_t task_info = GetTaskInfo(task_id);
    Priority_t priority = LOW_PRIORITY;
    if (task_info.required_sla == SLA0 || task_info.required_sla == SLA1) {
        priority = HIGH_PRIORITY;
    } else if (task_info.required_sla == SLA2) {
        priority = MID_PRIORITY;
    }

    VMId_t best_vm = VMId_t(UINT_MAX);
    double best_utilization = 0.0;
    bool found_vm = false;

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineId_t machine_id = machine_order[i];
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        int index = findMachineIndex(machine_id);
        if (index == -1) {
            continue;
        }

        const MachineData& cached = machine_data[index];
        if (machine_info.s_state == S0 &&
            cached.cpu_type == task_info.required_cpu &&
            (!task_info.gpu_capable || cached.gpu)) {

            if (machine_to_vms.find(machine_id) != machine_to_vms.end()) {
                vector<VMId_t>& vm_list = machine_to_vms[machine_id];
                for (unsigned j = 0; j < vm_list.size(); j++) {
                    VMId_t vm_id = vm_list[j];
                    if (migrating_vms.find(vm_id) == migrating_vms.end()) {
                        VMInfo_t vm_info = VM_GetInfo(vm_id);
                        if (vm_info.cpu == task_info.required_cpu && vm_info.vm_type == task_info.required_vm) {
                            if (machine_info.memory_used + task_info.required_memory <= cached.memory_size) {
                                double utilization = 1.0;
                                if (cached.num_cpus != 0) {
                                    utilization = double(machine_info.active_tasks + 1) / double(cached.num_cpus);
                                }

                                bool acceptable = true;
                                if (task_info.required_sla == SLA0 && utilization > 0.70) {
                                    acceptable = false;
                                }
                                if (task_info.required_sla == SLA1 && utilization > 0.85) {
                                    acceptable = false;
                                }
                                if (task_info.required_sla == SLA2 && utilization > 0.95) {
                                    acceptable = false;
                                }

                                if (acceptable) {
                                    if (!found_vm || utilization < best_utilization) {
                                        found_vm = true;
                                        best_utilization = utilization;
                                        best_vm = vm_id;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (found_vm) {
        VM_AddTask(best_vm, task_id, priority);
        task_to_vm[task_id] = best_vm;
        MachineId_t machine_id = VM_GetInfo(best_vm).machine_id;
        idle_since.erase(machine_id);
        return;
    }

    MachineId_t best_machine = MachineId_t(UINT_MAX);
    double best_machine_utilization = 0.0;
    bool found_machine = false;

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineId_t machine_id = machine_order[i];
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        int index = findMachineIndex(machine_id);
        if (index == -1) {
            continue;
        }

        const MachineData& cached = machine_data[index];
        bool vm_ok = true;
        if (task_info.required_vm == AIX && cached.cpu_type != POWER) {
            vm_ok = false;
        }
        if (task_info.required_vm == WIN && cached.cpu_type != ARM && cached.cpu_type != X86) {
            vm_ok = false;
        }

        if (!machine_data[index].state_changing &&
            machine_info.s_state == S0 &&
            cached.cpu_type == task_info.required_cpu &&
            vm_ok &&
            (!task_info.gpu_capable || cached.gpu) &&
            machine_info.memory_used + task_info.required_memory + VM_MEMORY_OVERHEAD <= cached.memory_size) {

            double utilization = 1.0;
            if (cached.num_cpus != 0) {
                utilization = double(machine_info.active_tasks + 1) / double(cached.num_cpus);
            }

            bool acceptable = true;
            if (task_info.required_sla == SLA0 && utilization > 0.75) {
                acceptable = false;
            }
            if (task_info.required_sla == SLA1 && utilization > 0.90) {
                acceptable = false;
            }
            if (task_info.required_sla == SLA2 && utilization > 1.00) {
                acceptable = false;
            }

            if (acceptable) {
                if (!found_machine || utilization < best_machine_utilization) {
                    found_machine = true;
                    best_machine_utilization = utilization;
                    best_machine = machine_id;
                }
            }
        }
    }

    if (found_machine) {
        VMId_t vm_id = VM_Create(task_info.required_vm, task_info.required_cpu);
        VM_Attach(vm_id, best_machine);
        VM_AddTask(vm_id, task_id, priority);
        task_to_vm[task_id] = vm_id;
        vm_to_machine[vm_id] = best_machine;
        machine_to_vms[best_machine].push_back(vm_id);
        vms.push_back(vm_id);
        idle_since.erase(best_machine);
        return;
    }

    if (pending_set.find(task_id) == pending_set.end()) {
        pending_tasks.push(task_id);
        pending_set.insert(task_id);
    }

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineId_t machine_id = machine_order[i];
        int index = findMachineIndex(machine_id);
        if (index == -1) {
            continue;
        }

        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        const MachineData& cached = machine_data[index];
        bool vm_ok = true;
        if (task_info.required_vm == AIX && cached.cpu_type != POWER) {
            vm_ok = false;
        }
        if (task_info.required_vm == WIN && cached.cpu_type != ARM && cached.cpu_type != X86) {
            vm_ok = false;
        }

        if (cached.cpu_type == task_info.required_cpu &&
            vm_ok &&
            machine_info.memory_used + task_info.required_memory + VM_MEMORY_OVERHEAD <= cached.memory_size &&
            (!task_info.gpu_capable || cached.gpu) &&
            machine_info.s_state != S0 &&
            !machine_data[index].state_changing) {

            Machine_SetState(machine_id, S0);
            machine_data[index].state_changing = true;
            machine_data[index].target_state = S0;
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

        if (!IsTaskCompleted(task_id) && task_to_vm.find(task_id) == task_to_vm.end()) {
            NewTask(now, task_id);
        }
    }

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineId_t machine_id = machine_order[i];
        int index = findMachineIndex(machine_id);
        if (index == -1) {
            continue;
        }

        if (!machine_data[index].state_changing) {
            MachineInfo_t info = Machine_GetInfo(machine_id);
            if (info.active_tasks == 0) {
                if (idle_since.find(machine_id) == idle_since.end()) {
                    idle_since[machine_id] = now;
                }
            } else {
                idle_since.erase(machine_id);
            }

            if (machine_to_vms.find(machine_id) != machine_to_vms.end()) {
                vector<VMId_t> kept;
                vector<VMId_t>& vm_list = machine_to_vms[machine_id];
                for (unsigned j = 0; j < vm_list.size(); j++) {
                    VMId_t vm_id = vm_list[j];
                    if (migrating_vms.find(vm_id) != migrating_vms.end()) {
                        kept.push_back(vm_id);
                    } else {
                        VMInfo_t vm_info = VM_GetInfo(vm_id);
                        if (vm_info.active_tasks.empty()) {
                            vm_to_machine.erase(vm_id);
                            vms.erase(remove(vms.begin(), vms.end(), vm_id), vms.end());
                            VM_Shutdown(vm_id);
                        } else {
                            kept.push_back(vm_id);
                        }
                    }
                }
                vm_list.swap(kept);
                if (vm_list.empty()) {
                    machine_to_vms.erase(machine_id);
                }
            }

            if (idle_since.find(machine_id) != idle_since.end()) {
                Time_t idle = now - idle_since[machine_id];
                if (idle >= S2_TO_S5 && info.s_state == S2) {
                    Machine_SetState(machine_id, S5);
                    machine_data[index].state_changing = true;
                    machine_data[index].target_state = S5;
                } else if (idle >= IDLE_TO_S2 && info.s_state == S0) {
                    Machine_SetState(machine_id, S2);
                    machine_data[index].state_changing = true;
                    machine_data[index].target_state = S2;
                }
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

    if (migrating_vms.find(vm_id) == migrating_vms.end()) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if (vm_info.active_tasks.empty()) {
            MachineId_t machine_id = vm_info.machine_id;
            if (machine_to_vms.find(machine_id) != machine_to_vms.end()) {
                vector<VMId_t>& vm_list = machine_to_vms[machine_id];
                vm_list.erase(remove(vm_list.begin(), vm_list.end(), vm_id), vm_list.end());
                if (vm_list.empty()) {
                    machine_to_vms.erase(machine_id);
                }
            }
            vm_to_machine.erase(vm_id);
            vms.erase(remove(vms.begin(), vms.end(), vm_id), vms.end());
            VM_Shutdown(vm_id);
            idle_since[machine_id] = now;
        }
    }

    SimOutput("TaskComplete(): task=" + to_string(task_id) + " completed at " + to_string(now), 3);
}

static Scheduler eco_scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    eco_scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    eco_scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    eco_scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);

    MachineInfo_t info = Machine_GetInfo(machine_id);
    for (unsigned i = 0; i < machine_order.size(); i++) {
        int index = findMachineIndex(machine_order[i]);
        if (index == -1) {
            continue;
        }

        MachineInfo_t candidate = Machine_GetInfo(machine_order[i]);

        if (candidate.machine_id != machine_id && candidate.cpu == info.cpu && candidate.s_state != S0 &&
            candidate.memory_size >= info.memory_used && !machine_data[index].state_changing) {

            Machine_SetState(candidate.machine_id, S0);
            machine_data[index].state_changing = true;
            machine_data[index].target_state = S0;
            break;
        }
    }
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    eco_scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    eco_scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time) / 1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);

    eco_scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);

    if (task_to_vm.find(task_id) != task_to_vm.end()) {
        VMId_t vm_id = task_to_vm[task_id];
        if (migrating_vms.find(vm_id) == migrating_vms.end()) {
            VMInfo_t vm_info = VM_GetInfo(vm_id);
            MachineInfo_t current_info = Machine_GetInfo(vm_info.machine_id);

            if (current_info.active_tasks >= current_info.num_cpus || current_info.memory_used >= current_info.memory_size) {
                for (unsigned i = 0; i < machine_order.size(); i++) {
                    MachineId_t machine_id = machine_order[i];
                    int index = findMachineIndex(machine_id);
                    if (index == -1) {
                        continue;
                    }

                    MachineInfo_t info = Machine_GetInfo(machine_id);

                    if (machine_id != vm_info.machine_id && info.cpu == current_info.cpu && info.s_state == S0 &&
                        info.active_tasks < info.num_cpus && info.memory_used + VM_MEMORY_OVERHEAD < info.memory_size && 
                        !machine_data[index].state_changing) {

                        migrating_vms.insert(vm_id);
                        VM_Migrate(vm_id, machine_id);
                        return;
                    }
                }
            }
        }
    }

    TaskInfo_t task_info = GetTaskInfo(task_id);
    if (pending_set.find(task_id) == pending_set.end()) {
        pending_tasks.push(task_id);
        pending_set.insert(task_id);
    }

    for (unsigned i = 0; i < machine_order.size(); i++) {
        MachineId_t machine_id = machine_order[i];
        int index = findMachineIndex(machine_id);
        if (index == -1) {
            continue;
        }

        MachineInfo_t info = Machine_GetInfo(machine_id);
        if (info.cpu == task_info.required_cpu && info.s_state != S0 && !machine_data[index].state_changing) {
            Machine_SetState(machine_id, S0);
            machine_data[index].state_changing = true;
            machine_data[index].target_state = S0;
            return;
        }
    }
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    int index = findMachineIndex(machine_id);
    if (index != -1) {
        machine_data[index].state_changing = false;
        machine_data[index].s_state = Machine_GetInfo(machine_id).s_state;
    }

    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.s_state == S0 && info.active_tasks == 0) {
        idle_since[machine_id] = Now();
    }
}