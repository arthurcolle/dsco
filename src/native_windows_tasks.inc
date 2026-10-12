/* Included after the native window model helpers. All queue/model access uses
 * mu; execution is performed by the service only after this lock is released. */
static native_window_task_t task_queue[NATIVE_WINDOWS_MAX];
static bool task_used[NATIVE_WINDOWS_MAX];
static uint64_t task_epoch=1,next_task_sequence=1;
static pthread_cond_t task_ready=PTHREAD_COND_INITIALIZER;
static void task_reset_locked(void) {
    memset(task_used,0,sizeof(task_used));task_epoch++;
    native_apps_interrupt();
    pthread_cond_broadcast(&task_ready);
}
bool native_window_is_editable(const native_window_t *w) {
    return w && (w->kind==NATIVE_WINDOW_BUFFER || w->kind==NATIVE_WINDOW_TASK);
}
const char *native_window_display_text(const native_window_t *w) {
    return w && w->kind==NATIVE_WINDOW_TASK && w->task_show_result ? w->task_output : w ? w->text : "";
}
static bool submit_task_locked(native_window_t *w) {
    if(!w || w->kind!=NATIVE_WINDOW_TASK)return false;
    if((w->task_pending||w->task_running) && w->task_generation==w->edit_generation) {
        state.local_task_coalesced++;return true;
    }
    native_factory_t factory;
    native_app_t app;
    sync_app_locked(w,true);
    if(w->app_id && native_apps_get(w->app_id,&app))factory=app.factory;
    else if(!native_factory_lookup(w->factory,&factory)){feedback("Local factory not found.");return false;}
    int slot=-1;
    for(int i=0;i<NATIVE_WINDOWS_MAX;i++)if(task_used[i] && (task_queue[i].window_id==w->id || (w->app_id && task_queue[i].app_id==w->app_id))){slot=i;state.local_task_coalesced++;break;}
    if(slot<0)for(int i=0;i<NATIVE_WINDOWS_MAX;i++)if(!task_used[i]){slot=i;break;}
    if(slot<0){feedback("Local executor queue is full; no request was added.");return false;}
    native_window_task_t *r=&task_queue[slot];memset(r,0,sizeof(*r));
    r->window_id=w->id;r->epoch=task_epoch;r->sequence=next_task_sequence++;
    r->edit_generation=w->edit_generation;r->factory=factory;
    memcpy(r->input,w->text,sizeof(r->input));r->input[sizeof(r->input)-1]=0;
    r->app_id=w->app_id;
    if(r->app_id){r->app_generation=native_apps_begin(r->app_id,r->sequence);if(!r->app_generation)return false;}
    task_used[slot]=true;w->task_sequence=r->sequence;w->task_generation=r->edit_generation;
    w->task_pending=true;w->status=NATIVE_WINDOW_RUNNING;
    text_copy(w->next_step,sizeof(w->next_step),"Native local execution; no model request.",false);
    w->revision++;changed();pthread_cond_signal(&task_ready);return true;
}
bool native_windows_submit_task(uint64_t id) {
    pthread_mutex_lock(&mu);int i=find_id(id);bool ok=submit_task_locked(i>=0?&state.windows[i]:NULL);pthread_mutex_unlock(&mu);return ok;
}
static bool take_task_locked(native_window_task_t *out) {
    int slot=-1;
    for(int i=0;i<NATIVE_WINDOWS_MAX;i++)if(task_used[i] && (slot<0||task_queue[i].sequence<task_queue[slot].sequence))slot=i;
    if(slot<0)return false;
    *out=task_queue[slot];task_used[slot]=false;state.local_task_started++;
    int i=find_id(out->window_id);
    if(i>=0 && state.windows[i].task_sequence==out->sequence){state.windows[i].task_pending=false;state.windows[i].task_running=true;}
    return true;
}
bool native_windows_take_task(native_window_task_t *out) {
    if(!out)return false;
    pthread_mutex_lock(&mu);bool ok=take_task_locked(out);pthread_mutex_unlock(&mu);return ok;
}
bool native_windows_wait_task(native_window_task_t *out,int timeout_ms) {
    if(!out)return false;
    if(timeout_ms<0)timeout_ms=0;if(timeout_ms>1000)timeout_ms=1000;
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=(long)(timeout_ms%1000)*1000000L;until.tv_sec+=timeout_ms/1000+until.tv_nsec/1000000000L;until.tv_nsec%=1000000000L;
    pthread_mutex_lock(&mu);bool ok=take_task_locked(out);
    if(!ok && timeout_ms){pthread_cond_timedwait(&task_ready,&mu,&until);ok=take_task_locked(out);}
    pthread_mutex_unlock(&mu);return ok;
}
void native_windows_wake_tasks(void){pthread_mutex_lock(&mu);pthread_cond_broadcast(&task_ready);pthread_mutex_unlock(&mu);}
bool native_windows_complete_task(const native_window_task_t *r,const native_factory_result_t *result) {
    if(!r||!result)return false;
    pthread_mutex_lock(&mu);int i=find_id(r->window_id);
    if(r->app_id){i=-1;for(int j=0;j<state.count;j++)if(state.windows[j].app_id==r->app_id){i=j;break;}}
    native_window_t *w=i>=0?&state.windows[i]:NULL;
    sync_app_locked(w,true);
    bool current=r->epoch==task_epoch && (r->app_id ?
        native_apps_complete(r->app_id,r->sequence,r->app_generation,result) :
        w && w->kind==NATIVE_WINDOW_TASK && w->task_sequence==r->sequence && w->edit_generation==r->edit_generation);
    if(!current){
        state.local_task_stale++;
        if(w && w->task_sequence==r->sequence && r->epoch==task_epoch){w->task_running=false;w->status=NATIVE_WINDOW_READY;text_copy(w->next_step,sizeof(w->next_step),"Input changed; old result discarded. Ctrl-R runs current input.",false);w->revision++;changed();}
        pthread_mutex_unlock(&mu);return false;
    }
    if(!w){state.local_task_completed++;changed();pthread_mutex_unlock(&mu);return true;}
    w->task_running=false;w->task_pending=false;w->status=result->ok?NATIVE_WINDOW_DONE:NATIVE_WINDOW_BLOCKED;
    text_copy(w->task_output,sizeof(w->task_output),result->output,true);
    text_copy(w->task_input_hash,sizeof(w->task_input_hash),result->input_sha256,false);
    snprintf(w->evidence,sizeof(w->evidence),"Native C; %llu us; request %llu; input SHA %.16s",(unsigned long long)result->elapsed_us,(unsigned long long)r->sequence,result->input_sha256);
    text_copy(w->next_step,sizeof(w->next_step),result->ok?(w->app_id?"Result retained by the application. Closing detaches the view.":"Result is local. Input edits are session-only; Ctrl-R reruns."):"Invalid input. Choose Input, fix it, then Ctrl-R.",false);
    w->task_show_result=true;w->editor_active=false;w->scroll=0;w->revision++;
    state.local_task_completed++;changed();pthread_mutex_unlock(&mu);return true;
}
static void task_action_locked(native_window_t *w,int button) {
    if(button==0){submit_task_locked(w);return;}
    if(button==1){w->task_show_result=false;w->editor_active=true;state.keyboard_focus=true;}
    else if(button==2){w->task_show_result=true;w->editor_active=false;}
    else if(button==3){
        for(int i=0;i<NATIVE_WINDOWS_MAX;i++)if(task_used[i] && (task_queue[i].window_id==w->id || (w->app_id && task_queue[i].app_id==w->app_id)))task_used[i]=false;
        if(w->app_id)native_apps_clear(w->app_id);
        w->task_sequence=next_task_sequence++;w->task_pending=false;w->task_running=false;w->task_output[0]=0;w->task_show_result=false;w->editor_active=true;w->status=NATIVE_WINDOW_READY;
    }
    w->scroll=0;w->revision++;changed();
}
