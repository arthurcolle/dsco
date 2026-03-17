# Escape Key Handling System — Complete Design & Implementation

**Date:** 2026-03-13  
**Purpose:** Sophisticated pause/cancel system for dsco workflows  
**Status:** Design complete, implementation ready

---

## 1. Executive Summary

Implement a two-state escape key system that allows users to pause and resume workflows with complete context preservation. First ESC pauses, second ESC confirms cancellation.

**Key Features:**
- ✅ Pause without losing state (first ESC)
- ✅ Cancel with confirmation (second ESC)
- ✅ Full context preservation during pause
- ✅ Agent state snapshots
- ✅ IPC channel preservation
- ✅ Automatic backup on pause/cancel
- ✅ Resume exactly where paused

---

## 2. System Architecture

### 2.1 State Machine

```
ESCAPE_RUNNING
    ↓ [ESC pressed]
ESCAPE_PAUSED (show pause menu, preserve context)
    ├─ [R] Resume → back to ESCAPE_RUNNING
    ├─ [S] Save → save state file
    ├─ [C] → show confirmation
    └─ [V] View → show progress details
        ↓ [ESC pressed again]
ESCAPE_CANCEL_PENDING (ask for confirmation)
    ├─ [ESC pressed] → ESCAPE_CANCELLED (cleanup & exit)
    └─ [Any other key] → back to ESCAPE_PAUSED
```

### 2.2 Context Preservation During Pause

When workflow pauses, save:

```c
pause_context_t {
    // Workflow position
    int current_step;
    int total_steps;
    char current_task[1024];
    
    // Execution timeline
    struct {
        double started_at;
        double paused_at;
        double resumed_at;
        double duration_paused;
    } timing;
    
    // Agent snapshots (frozen state)
    agent_snapshot_t agents[100];
    int agent_count;
    
    // Preserved state
    char context_file[512];      // LLM context snapshot
    char ipc_message_queue[16K]; // Unprocessed IPC messages
    char results_so_far[4KB];    // Partial results
    
    // Backup file for recovery
    char backup_file[512];
}
```

### 2.3 Pause Flow (Detailed)

```
┌────────────────────────────────────┐
│ Workflow executing at step N       │
├────────────────────────────────────┤
│ User presses ESC                   │
├────────────────────────────────────┤
│ Signal handler triggered (SIGINT)  │
│ Transition to ESCAPE_PAUSED        │
├────────────────────────────────────┤
│ SAVE CONTEXT:                      │
│  1. Snapshot all agent states      │
│  2. Capture IPC channel status     │
│  3. Save pending messages to file  │
│  4. Export workflow state JSON     │
│  5. Create backup file             │
├────────────────────────────────────┤
│ FREEZE EXECUTION:                  │
│  1. Pause all running agents (SIGSTOP)
│  2. Stop accepting new work items  │
│  3. Keep IPC channels open         │
│  4. Don't close file descriptors   │
├────────────────────────────────────┤
│ SHOW PAUSE MENU                    │
│  • Current step: N of M            │
│  • Duration: HH:MM:SS              │
│  • Agents: X running, Y pending    │
│  • Options: Resume, Save, Cancel   │
├────────────────────────────────────┤
│ WAIT FOR USER INPUT                │
├────────────────────────────────────┤
│ Option selected:                   │
│  [R] Resume → Restore & continue   │
│  [S] Save → Save & exit gracefully │
│  [C] → Show cancel confirmation    │
│  [V] → Show detailed progress      │
└────────────────────────────────────┘
```

### 2.4 Cancel Flow (Detailed)

```
┌────────────────────────────────────┐
│ Workflow paused                    │
│ Show cancel confirmation menu      │
├────────────────────────────────────┤
│ User presses ESC again             │
├────────────────────────────────────┤
│ Transition to ESCAPE_CANCELLED     │
├────────────────────────────────────┤
│ CLEANUP PHASE:                     │
│  1. Kill all running agents        │
│     (send SIGTERM, wait, SIGKILL)  │
│  2. Close agent pipes/IPC channels │
│  3. Flush message queue            │
│  4. Release allocated memory       │
│  5. Clear temporary files          │
├────────────────────────────────────┤
│ SAVE FINAL STATE:                  │
│  1. Write cancel marker to backup  │
│  2. Save completed steps count     │
│  3. Archive partial results        │
│  4. Log cancellation reason        │
├────────────────────────────────────┤
│ REPORT CANCELLATION:               │
│  • Agents killed: X                │
│  • Steps completed: N of M         │
│  • Backup location: /path/to/file  │
│  • Can resume later: Yes/No        │
├────────────────────────────────────┤
│ Exit cleanly                       │
└────────────────────────────────────┘
```

---

## 3. Data Structures

### 3.1 Pause Context

```c
typedef struct {
    // Workflow metadata
    char workflow_name[256];
    char workflow_id[64];
    int current_step;
    int total_steps;
    char current_task[1024];
    
    // Timing information
    struct {
        double started_at;      // ms since epoch
        double paused_at;
        double resumed_at;
        double duration_paused;
    } timing;
    
    // Agent snapshots (frozen state)
    struct {
        int agent_id;
        int pid;
        int state;              // RUNNING, PAUSED, COMPLETED, FAILED
        char role[64];
        char last_message[512];
        double started_at;
    } agents[100];
    int agent_count;
    
    // Preserved context
    char context_snapshot[128 * 1024];  // LLM context
    size_t context_size;
    
    // IPC state
    struct {
        int pending_messages;
        int channels_active;
        char topics[256][64];
        char message_queue[16384];
    } ipc_state;
    
    // Results accumulated so far
    char results_summary[4096];
    int results_size;
    
    // Backup & recovery
    char backup_file[512];
    int backup_complete;
    
} pause_context_t;
```

### 3.2 Escape State Machine

```c
typedef enum {
    ESCAPE_RUNNING = 0,              // Normal execution
    ESCAPE_PAUSED = 1,               // Paused by user (1st ESC)
    ESCAPE_CANCEL_PENDING = 2,       // Awaiting cancel confirmation
    ESCAPE_CANCELLED = 3             // Cancelled (2nd ESC)
} escape_state_t;
```

### 3.3 Agent Snapshot

```c
typedef struct {
    int agent_id;
    int pid;
    int state;                       // RUNNING, PAUSED, COMPLETED, FAILED
    char role[64];
    char last_message[512];
    double started_at;
    double duration_at_pause;        // How long it ran before pause
} agent_snapshot_t;
```

---

## 4. API Reference

### Initialization

```c
int escape_init(const char *workflow_name, int total_steps);
```
Initialize escape handler for a workflow.

### Workflow Progress Tracking

```c
void escape_set_step(int step, const char *task_description);
```
Update current step and task description.

### Agent Management

```c
void escape_add_agent(int agent_id, int pid, const char *role);
void escape_remove_agent(int agent_id);
```
Register/unregister agents with the pause system.

### State Queries

```c
escape_state_t escape_get_state(void);
int escape_is_paused(void);
int escape_is_cancelled(void);
```
Query current escape state.

### Control

```c
void escape_resume(void);
void escape_save_and_exit(void);
void escape_cancel(void);
```
Control workflow execution.

### Context Management

```c
int escape_save_context(const char *backup_path);
int escape_load_context(const char *backup_path);
char* escape_get_backup_file(void);
```
Save/load workflow state.

---

## 5. Implementation Steps

### Phase 1: Signal Handler (Week 1)

```c
// Install signal handler
signal(SIGINT, escape_signal_handler);

// Implement state machine transitions
// Handle debouncing (ignore <100ms repeats)
```

**Deliverable:** Basic pause/cancel flow

### Phase 2: Context Preservation (Week 2)

```c
// Snapshot workflow state
void save_pause_context();

// Freeze agents (SIGSTOP)
void freeze_agents();

// Preserve IPC channels
void snapshot_ipc_state();

// Save backup to JSON
void write_backup_file();
```

**Deliverable:** Full state preservation

### Phase 3: Resume Capability (Week 3)

```c
// Restore workflow state
void restore_pause_context();

// Resume agents (SIGCONT)
void resume_agents();

// Restore IPC channels
void restore_ipc_state();

// Continue from step N
void resume_from_step();
```

**Deliverable:** Pause and resume working

### Phase 4: UI & Menus (Week 4)

```c
// Show pause menu
void show_pause_menu();

// Show cancel confirmation
void show_cancel_confirmation();

// Show progress details
void show_progress_details();

// Handle user input
void handle_pause_menu_input();
```

**Deliverable:** Professional UI, user-friendly

---

## 6. Integration with dsco

### 6.1 Workflow System Integration

In `workflow.c`:

```c
// Initialize escape handler on workflow start
void workflow_execute(workflow_t *wf) {
    escape_init(wf->name, wf->step_count);
    
    for (int i = 0; i < wf->step_count; i++) {
        // Check if cancelled
        if (escape_is_cancelled()) {
            workflow_save_state(wf, i);
            return;
        }
        
        escape_set_step(i, wf->steps[i]->description);
        
        // Execute step
        workflow_step_execute(wf->steps[i]);
        
        // Check if paused
        while (escape_is_paused()) {
            sleep(100);  // Wait for user input
        }
    }
}
```

### 6.2 Agent System Integration

In `agent.c`:

```c
// Register agent with escape system
void agent_spawn(swarm_t *swarm, agent_task_t *task) {
    int agent_id = agent_create();
    int pid = fork();
    
    if (pid == 0) {
        // Child process
        agent_execute(agent_id, task);
    } else {
        // Parent: register with escape handler
        escape_add_agent(agent_id, pid, task->role);
    }
}
```

### 6.3 IPC Integration

In `ipc.c`:

```c
// Snapshot IPC state on pause
void ipc_snapshot() {
    // Get pending message count
    // Save unprocessed messages to file
    // Preserve channel state
}

// Restore IPC state on resume
void ipc_restore() {
    // Reopen channels if needed
    // Restore pending messages
    // Resume message processing
}
```

---

## 7. User Experience Examples

### Example 1: Pause and Resume

```
🚀 Starting workflow: code_analysis
Total steps: 10
(Press Ctrl+C to pause)

[1/10] Analyzing file 1/10 ✓
[2/10] Analyzing file 2/10 ✓
[3/10] Analyzing file 3/10 ^C

✋ Pausing workflow...

┌─────────────────────────────────────────┐
│  ⏸️  WORKFLOW PAUSED                     │
├─────────────────────────────────────────┤
│ Workflow:  code_analysis                │
│ Step:      3 of 10                      │
│ Duration:  2m 34s                       │
│ Task:      Analyzing file 3/10          │
│ Agents:    2 active                     │
├─────────────────────────────────────────┤
│ [R] Resume  [S] Save  [C] Cancel  [V] View
└─────────────────────────────────────────┘

> R

▶️  Resuming workflow from step 3...

[3/10] Analyzing file 3/10 ✓
[4/10] Analyzing file 4/10 ✓
...
[10/10] Analyzing file 10/10 ✓

✅ Workflow completed successfully
```

### Example 2: Pause and Cancel

```
[5/10] Analyzing file 5/10 ^C

✋ Pausing workflow...

┌─────────────────────────────────────────┐
│  ⏸️  WORKFLOW PAUSED                     │
├─────────────────────────────────────────┤
│ Workflow:  code_analysis                │
│ Step:      5 of 10                      │
│ Duration:  4m 12s                       │
│ Task:      Analyzing file 5/10          │
│ Agents:    3 active                     │
├─────────────────────────────────────────┤
│ [R] Resume  [S] Save  [C] Cancel  [V] View
└─────────────────────────────────────────┘

> C

┌─────────────────────────────────────────┐
│  ⚠️  CANCEL CONFIRMATION                 │
├─────────────────────────────────────────┤
│ Press ESC again to confirm cancel      │
│ Progress will be saved to:             │
│ /tmp/workflow_pause_code_analysis_...  │
│                                         │
│ [Any other key] Return to pause menu   │
└─────────────────────────────────────────┘

^C

❌ WORKFLOW CANCELLED

Cleanup:
  ✓ Killed 3 agent(s)
  ✓ Closed IPC channels
  ✓ Flushed message queues

Progress saved to:
/tmp/workflow_pause_code_analysis_20260313_141523.json

Results captured:
  • Completed steps: 4
  • Agents finished: 1
  • Partial findings: [summary]
```

---

## 8. Error Handling

| Scenario | Handling |
|----------|----------|
| Signal handler during agent fork | Queue signal, process after fork completes |
| Pause while agent is allocating memory | Wait for allocation, then pause |
| IPC deadlock on pause | Timeout on snapshot (5 sec), force cleanup |
| Agent doesn't respond to SIGSTOP | Send SIGKILL if SIGSTOP times out |
| Backup file write fails | Log error, proceed with in-memory context |
| Resume with missing context | Recreate from last saved state |

---

## 9. Testing Strategy

### Unit Tests

```c
// Test state machine transitions
test_escape_state_transitions();

// Test context saving/loading
test_pause_context_save();
test_pause_context_load();

// Test agent snapshots
test_agent_snapshot_capture();

// Test signal handling
test_signal_debouncing();
test_signal_timing();
```

### Integration Tests

```c
// Test with real workflow
test_workflow_pause_resume();

// Test with multiple agents
test_multiagent_pause();

// Test IPC preservation
test_ipc_pause_resume();

// Test context recovery
test_context_recovery_after_cancel();
```

### Stress Tests

```c
// Rapid ESC presses
test_rapid_escape_presses();

// Long pause duration
test_pause_duration_hours();

// Large context preservation
test_large_context_100mb();

// Many agents
test_100_agents_pause_resume();
```

---

## 10. Deployment Checklist

- [ ] Implement signal handler (SIGINT)
- [ ] Implement state machine
- [ ] Implement pause context preservation
- [ ] Implement agent snapshot system
- [ ] Implement IPC state preservation
- [ ] Implement resume mechanism
- [ ] Add pause menu UI
- [ ] Add user input handling
- [ ] Add error handling for edge cases
- [ ] Integrate with workflow system
- [ ] Integrate with agent system
- [ ] Integrate with IPC system
- [ ] Add unit tests
- [ ] Add integration tests
- [ ] Add documentation
- [ ] User acceptance testing

---

## 11. Performance Considerations

- **Pause latency:** <100ms from ESC press to pause menu visible
- **Resume latency:** <500ms from Resume selected to workflow continuing
- **Context snapshot:** <1 sec for 100 agents + 128KB context
- **Backup file:** <2 sec to write even for large contexts
- **Memory overhead:** ~1MB per paused workflow (backup structures)

---

## 12. Future Enhancements

- [ ] Pause history (view previous pauses)
- [ ] Automatic pause on error
- [ ] Pause on resource exhaustion (CPU, memory)
- [ ] Distributed pause (pause all agents in mesh)
- [ ] Pause hooks (allow custom pause handlers)
- [ ] Pause middleware (logging, analytics)
- [ ] Web UI for pause management

---

## Conclusion

The escape key system provides sophisticated workflow control with complete context preservation. Users can pause workflows without losing progress, inspect state, and either resume or cancel gracefully.

**Key Benefits:**
- ✅ Non-destructive pause (first ESC)
- ✅ Safe cancellation (second ESC with confirmation)
- ✅ Full context preservation
- ✅ Agent state snapshots
- ✅ IPC channel preservation
- ✅ Automatic backups
- ✅ Professional UX

**Implementation Effort:** 4 weeks, 1-2 developers

---

**Document Status:** Ready for Implementation  
**Last Updated:** 2026-03-13
