//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//  Greedy Algorithm
//

#include <cstdint>
#include "Scheduler.hpp"

#include <map>
#include <set>
#include <queue>
#include <algorithm>

struct MachineData {
    MachineId_t machine_id;
    unsigned memory_size;
    unsigned num_cpus;
    bool gpu;
    
    CPUType_t cpu_type;
    MachineState_t s_state;

    bool state_changing;
    MachineState_t target_state;
};

vector<MachineData> machine_data;
map<TaskId_t, VMId_t> task_to_vm;
static queue<TaskId_t> pending_tasks;
static set<TaskId_t> pending_set;

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Initializing scheduler", 3);
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

        machines.push_back(info.machine_id);
        machine_data[i] = data;
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    if (task_to_vm.find(task_id) != task_to_vm.end()) return;
    TaskInfo_t task_info = GetTaskInfo(task_id);
    Priority_t priority = MID_PRIORITY;
    if (task_info.required_sla == SLA0 || task_info.required_sla == SLA1) {
        priority = HIGH_PRIORITY;
    } else if (task_info.required_sla == SLA3) {
        priority = LOW_PRIORITY;
    }

    for (unsigned i = 0; i < vms.size(); i++) {
        VMInfo_t vm_info = VM_GetInfo(vms[i]);
        if (vm_info.cpu == task_info.required_cpu && vm_info.vm_type == task_info.required_vm) {
            MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);

            bool gpu_ok = !task_info.gpu_capable || machine_info.gpus;
            bool mem_ok = machine_info.memory_used + task_info.required_memory <= machine_info.memory_size;
            bool capacity_ok = false;

            if (task_info.required_sla == SLA0) {
                capacity_ok = machine_info.active_tasks + 1 < machine_info.num_cpus;
            } else if (task_info.required_sla == SLA1) {
                capacity_ok = machine_info.active_tasks < machine_info.num_cpus;
            } else if (task_info.required_sla == SLA2) {
                capacity_ok = machine_info.active_tasks <= machine_info.num_cpus;
            } else {
                capacity_ok = vm_info.active_tasks.size() < machine_info.num_cpus * 2;
            }

            if (machine_info.s_state == S0 && gpu_ok && mem_ok && capacity_ok) {
                VM_AddTask(vms[i], task_id, priority);
                task_to_vm[task_id] = vms[i];
                return;
            }
        }
    }

    for (unsigned i = 0; i < machines.size(); i++) {
        MachineInfo_t info = Machine_GetInfo(machines[i]);

        bool gpu_ok = !task_info.gpu_capable || info.gpus;
        bool mem_ok = info.memory_used + task_info.required_memory + VM_MEMORY_OVERHEAD <= info.memory_size;
        bool capacity_ok = false;

        if (task_info.required_sla == SLA0) {
            capacity_ok = info.active_tasks + 1 < info.num_cpus;
        } else if (task_info.required_sla == SLA1) {
            capacity_ok = info.active_tasks < info.num_cpus;
        } else if (task_info.required_sla == SLA2) {
            capacity_ok = info.active_tasks <= info.num_cpus;
        } else {
            capacity_ok = info.active_tasks < info.num_cpus * 2;
        }

        if (info.cpu == task_info.required_cpu && info.s_state == S0 && gpu_ok && mem_ok && capacity_ok) {
            VMId_t vm_id = VM_Create(task_info.required_vm, task_info.required_cpu);
            VM_Attach(vm_id, info.machine_id);
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

    for (unsigned i = 0; i < machines.size(); i++) {
        MachineInfo_t data = Machine_GetInfo(machines[i]);
        if (data.cpu == task_info.required_cpu &&(!task_info.gpu_capable || data.gpus) && data.s_state != S0 && data.memory_used + task_info.required_memory + VM_MEMORY_OVERHEAD <= data.memory_size && !machine_data[i].state_changing) {
            Machine_SetState(data.machine_id, S0);
            machine_data[i].state_changing = true;
            machine_data[i].target_state = S0;
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
}

void Scheduler::Shutdown(Time_t time) {
    for (unsigned i = 0; i < vms.size(); i++) {
        VMInfo_t vm_info = VM_GetInfo(vms[i]);
        if (vm_info.active_tasks.empty()) {
            VM_Shutdown(vms[i]);
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

    VMInfo_t vm_info = VM_GetInfo(vm_id);
    if (vm_info.active_tasks.empty()) {
        VM_Shutdown(vm_id);
        vms.erase(remove(vms.begin(), vms.end(), vm_id), vms.end());
    }

    SimOutput("TaskComplete(): task=" + to_string(task_id) + " completed at " + to_string(now), 3);
}

// Public interface below

static Scheduler greed_scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    greed_scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    greed_scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    greed_scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    greed_scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    greed_scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    
    greed_scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);

    TaskInfo_t task_info = GetTaskInfo(task_id);
    for (unsigned i = 0; i < machine_data.size(); i++) {
        MachineInfo_t info = Machine_GetInfo(machine_data[i].machine_id);
        if (info.cpu == task_info.required_cpu &&
            (!task_info.gpu_capable || info.gpus) && info.s_state != S0 && info.memory_used + task_info.required_memory + VM_MEMORY_OVERHEAD <= info.memory_size && !machine_data[i].state_changing) {
            Machine_SetState(info.machine_id, S0);
            machine_data[i].state_changing = true;
            machine_data[i].target_state = S0;
            break;
        }
    }
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    for (unsigned i = 0; i < machine_data.size(); i++) {
        if (machine_data[i].machine_id == machine_id) {
            machine_data[i].state_changing = false;
            machine_data[i].s_state = Machine_GetInfo(machine_id).s_state;
            break;
        }
    }
}