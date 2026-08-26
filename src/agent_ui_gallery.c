#define _POSIX_C_SOURCE 200809L

#include "agent_ui_gallery.h"

#include "agent_ui_canvas.h"
#include "agent_ui_components.h"
#include "kitty_graphics.h"
#include "px_widgets.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GALLERY_KEY_BASE UINT64_C(0x47414c4c00000000)

static uint64_t gallery_key(unsigned slot) {
    return GALLERY_KEY_BASE | (uint64_t)(slot + 1U);
}

static native_ui_node_t *add_node(native_ui_scene_t *scene, int parent,
                                  uint64_t key, native_ui_element_t element,
                                  native_ui_role_t role) {
    int index = native_ui_scene_add(scene, parent, key, element, role);
    return index >= 0 ? native_ui_scene_node(scene, index) : NULL;
}

static int node_index(const native_ui_scene_t *scene,
                      const native_ui_node_t *node) {
    return scene && node ? (int)(node - scene->nodes) : -1;
}

static void fixed_height(native_ui_node_t *node, int height) {
    if (!node) return;
    node->constraints.min_height = height;
    node->constraints.preferred_height = height;
    node->constraints.max_height = height;
    node->constraints.shrink = 0;
}

static void fixed_width(native_ui_node_t *node, int width) {
    if (!node) return;
    node->constraints.min_width = width;
    node->constraints.preferred_width = width;
    node->constraints.max_width = width;
    node->constraints.shrink = 0;
}

static native_ui_node_t *add_stack(native_ui_scene_t *scene, int parent,
                                   uint64_t key, int grow, int gap) {
    native_ui_node_t *node = add_node(scene, parent, key,
                                      NATIVE_UI_ELEMENT_STACK,
                                      NATIVE_UI_ROLE_NONE);
    if (!node) return NULL;
    node->style.flow = NATIVE_UI_FLOW_COLUMN;
    node->style.align = NATIVE_UI_ALIGN_STRETCH;
    node->style.gap = gap;
    node->constraints.grow = grow > 0 ? (uint16_t)grow : 0;
    return node;
}

static native_ui_node_t *add_columns(native_ui_scene_t *scene, int parent,
                                     uint64_t key, int gap) {
    native_ui_node_t *node = add_node(scene, parent, key,
                                      NATIVE_UI_ELEMENT_ROW,
                                      NATIVE_UI_ROLE_NONE);
    if (!node) return NULL;
    node->style.flow = NATIVE_UI_FLOW_ROW;
    node->style.align = NATIVE_UI_ALIGN_STRETCH;
    node->style.gap = gap;
    node->constraints.grow = 1;
    return node;
}

const char *agent_ui_gallery_page_name(agent_ui_gallery_page_t page) {
    static const char *names[] = {
        "Workbench", "Foundations", "Conversation", "Execution",
        "Orchestration", "Governance", "Themes",
    };
    return page >= 0 && page < AGENT_UI_GALLERY_PAGE_COUNT ? names[page]
                                                           : "Workbench";
}

static bool build_workbench(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene = builder->scene;
    const agent_ui_theme_t *theme = builder->theme;
    native_ui_node_t *columns = add_columns(scene, content, gallery_key(100),
                                            theme->spacing.lg);
    if (!columns) return false;
    native_ui_node_t *conversation = add_stack(scene, node_index(scene, columns),
                                               gallery_key(101), 5,
                                               theme->spacing.sm);
    native_ui_node_t *execution = add_stack(scene, node_index(scene, columns),
                                            gallery_key(102), 5,
                                            theme->spacing.sm);
    native_ui_node_t *inspector = add_stack(scene, node_index(scene, columns),
                                            gallery_key(103), 4,
                                            theme->spacing.sm);
    if (!conversation || !execution || !inspector) return false;
    int left = node_index(scene, conversation), middle = node_index(scene, execution);
    int right = node_index(scene, inspector);

    if (agent_ui_add_section_header(builder, left, gallery_key(104),
            "Live conversation", "retained transcript / streaming state", "TURN 12") < 0 ||
        agent_ui_add_message(builder, left,
            agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE, 1),
            &(agent_ui_message_model_t){AGENT_UI_MESSAGE_USER,"Arthur",
              "Build the terminal component system and prove it in Kitty.","14:42",false,false}) < 0 ||
        agent_ui_add_reasoning(builder, left,
            agent_ui_component_key(AGENT_UI_COMPONENT_REASONING, 1),
            &(agent_ui_reasoning_model_t){"Reasoning across the active workspace",
              "Mapping semantic components onto exact Retina pixels.","SYNTHESIZING",0.68f,true,false}) < 0 ||
        agent_ui_add_composer(builder, left,
            agent_ui_component_key(AGENT_UI_COMPONENT_COMPOSER, 1),
            &(agent_ui_composer_component_model_t){"","Command the agent…","AGENT","SEND",true,false}) < 0)
        return false;

    if (agent_ui_add_section_header(builder, middle, gallery_key(105),
            "Execution", "tools / plan / artifacts", "3 ACTIVE") < 0 ||
        agent_ui_add_tool(builder, middle,
            agent_ui_component_key(AGENT_UI_COMPONENT_TOOL, 1),
            &(agent_ui_tool_model_t){"exec_command","Compile component library",
              "make dsco-agent-ui-gallery","01.8s","RUNNING",AGENT_UI_TONE_ACCENT,0.72f,true}) < 0 ||
        agent_ui_add_plan_step(builder, middle,
            agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP, 1),
            &(agent_ui_plan_step_model_t){1,"Theme token catalog","16 validated themes","DONE",AGENT_UI_TONE_SUCCESS,false,false}) < 0 ||
        agent_ui_add_plan_step(builder, middle,
            agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP, 2),
            &(agent_ui_plan_step_model_t){2,"Agentic component primitives","semantic states + actions","ACTIVE",AGENT_UI_TONE_ACCENT,true,true}) < 0 ||
        agent_ui_add_plan_step(builder, middle,
            agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP, 3),
            &(agent_ui_plan_step_model_t){3,"Runtime integration","adopt incrementally","QUEUED",AGENT_UI_TONE_NEUTRAL,false,true}) < 0 ||
        agent_ui_add_artifact(builder, middle,
            agent_ui_component_key(AGENT_UI_COMPONENT_ARTIFACT, 1),
            &(agent_ui_artifact_model_t){"Agent UI component library","include/agent_ui_components.h",
              "Retained, accessible, backend-neutral C API","C","OPEN",true}) < 0)
        return false;

    if (agent_ui_add_section_header(builder, right, gallery_key(106),
            "Resource envelope", "live operational telemetry", "NATIVE") < 0 ||
        agent_ui_add_metric(builder, right,
            agent_ui_component_key(AGENT_UI_COMPONENT_METRIC, 1),
            &(agent_ui_metric_model_t){"CONTEXT","42.8","%","+3.1","18 24 20 32 39 43",AGENT_UI_TONE_ACCENT}) < 0 ||
        agent_ui_add_metric(builder, right,
            agent_ui_component_key(AGENT_UI_COMPONENT_METRIC, 2),
            &(agent_ui_metric_model_t){"TOOL LATENCY","184","ms","−22","42 38 35 31 27 24",AGENT_UI_TONE_SUCCESS}) < 0 ||
        agent_ui_add_agent_card(builder, right,
            agent_ui_component_key(AGENT_UI_COMPONENT_AGENT, 1),
            &(agent_ui_agent_model_t){"Overmind Soul","gpt-5.6-terra","Building UI foundations",
              "EXECUTING",NATIVE_UI_AGENT_EXECUTING,0.43f,true}) < 0 ||
        agent_ui_add_notification(builder, right,
            agent_ui_component_key(AGENT_UI_COMPONENT_NOTIFICATION, 1),
            &(agent_ui_notification_model_t){"Retina backing store","3706 × 2288 exact device pixels",
              "DETAILS",AGENT_UI_TONE_SUCCESS,true}) < 0)
        return false;
    return true;
}

static bool build_foundations(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene=builder->scene;
    const agent_ui_theme_t *theme=builder->theme;
    native_ui_node_t *columns=add_columns(scene,content,gallery_key(150),theme->spacing.lg);
    native_ui_node_t *status=columns?add_stack(scene,node_index(scene,columns),gallery_key(151),4,theme->spacing.sm):NULL;
    native_ui_node_t *data=columns?add_stack(scene,node_index(scene,columns),gallery_key(152),4,theme->spacing.sm):NULL;
    native_ui_node_t *activity=columns?add_stack(scene,node_index(scene,columns),gallery_key(153),4,theme->spacing.sm):NULL;
    if(!status||!data||!activity)return false;
    int a=node_index(scene,status),b=node_index(scene,data),c=node_index(scene,activity);
    if(agent_ui_add_section_header(builder,a,gallery_key(154),"Status foundations",
        "containers / values / progress / toast","6 PRIMITIVES")<0)return false;
    int card=px_widget_card(scene,a,gallery_key(155),"Session envelope");
    if(card<0)return false;fixed_height(native_ui_scene_node(scene,card),146);
    if(px_widget_kv_row(scene,card,gallery_key(156),"MODEL","gpt-5.6-terra")<0||
       px_widget_kv_row(scene,card,gallery_key(157),"CONTEXT","42.8%")<0||
       px_widget_kv_row(scene,card,gallery_key(158),"COST","$0.0842")<0)return false;
    native_ui_node_t *badges=add_node(scene,a,gallery_key(159),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_STATUS);
    if(!badges)return false;fixed_height(badges,28);badges->style.flow=NATIVE_UI_FLOW_ROW;badges->style.gap=theme->spacing.sm;
    int badge_parent=node_index(scene,badges);
    if(px_widget_badge(scene,badge_parent,gallery_key(160),"HEALTHY",PX_WIDGET_TONE_SUCCESS)<0||
       px_widget_badge(scene,badge_parent,gallery_key(161),"WAITING",PX_WIDGET_TONE_WARNING)<0||
       px_widget_badge(scene,badge_parent,gallery_key(162),"BLOCKED",PX_WIDGET_TONE_DANGER)<0||
       px_widget_progress(scene,a,gallery_key(163),0.68,0,PX_WIDGET_TONE_ACCENT)<0||
       px_widget_progress(scene,a,gallery_key(164),-1,0.42,PX_WIDGET_TONE_ACCENT)<0||
       px_widget_toast(scene,a,gallery_key(165),"Retained damage stayed inside the component",PX_WIDGET_TONE_SUCCESS)<0)return false;

    if(agent_ui_add_section_header(builder,b,gallery_key(166),"Data graphics",
        "meter / sparkline / gauge / bars","DEVICE SCALE")<0)return false;
    double trend[]={18,24,20,32,39,43,48,62};
    double bars[]={4,8,5,11,9,15,12,18};
    if(px_widget_meter(scene,b,gallery_key(167),"CONTEXT WINDOW",0.63,PX_WIDGET_TONE_ACCENT)<0||
       px_widget_sparkline(scene,b,gallery_key(168),trend,8,PX_WIDGET_TONE_SUCCESS)<0||
       px_widget_gauge(scene,b,gallery_key(169),"68%",0.68,PX_WIDGET_TONE_WARNING)<0||
       px_widget_bar_chart(scene,b,gallery_key(170),bars,8,PX_WIDGET_TONE_ACCENT)<0)return false;

    if(agent_ui_add_section_header(builder,c,gallery_key(171),"Liveness & identity",
        "spinner / reasoning pulse / agent state","ANIMATABLE")<0)return false;
    native_ui_node_t *live=add_node(scene,c,gallery_key(172),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_REASONING_ACTIVITY);
    if(!live)return false;fixed_height(live,52);live->style.flow=NATIVE_UI_FLOW_ROW;live->style.gap=theme->spacing.md;
    int live_parent=node_index(scene,live);
    int spinner=px_widget_spinner(scene,live_parent,gallery_key(173),0.31,PX_WIDGET_TONE_ACCENT);
    int dots=px_widget_activity_dots(scene,live_parent,gallery_key(174),0.72,PX_WIDGET_TONE_SUCCESS);
    if(spinner<0||dots<0)return false;
    fixed_width(native_ui_scene_node(scene,spinner),72);
    fixed_width(native_ui_scene_node(scene,dots),110);
    px_widget_agent_t agent={.name="Worker 03",.task="Running visual regression suite",
        .model="local/clang",.state=NATIVE_UI_AGENT_EXECUTING,.progress=0.55,.cost_usd=0.0124};
    if(px_widget_agent_card(scene,c,gallery_key(175),&agent)<0||
       px_widget_toast(scene,c,gallery_key(176),"Tool output is ready to inspect",PX_WIDGET_TONE_ACCENT)<0)return false;
    return true;
}

static bool build_conversation(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene = builder->scene;
    const agent_ui_theme_t *theme = builder->theme;
    native_ui_node_t *columns = add_columns(scene, content, gallery_key(200), theme->spacing.lg);
    native_ui_node_t *transcript = columns ? add_stack(scene, node_index(scene, columns),
        gallery_key(201), 7, theme->spacing.sm) : NULL;
    native_ui_node_t *rail = columns ? add_stack(scene, node_index(scene, columns),
        gallery_key(202), 4, theme->spacing.sm) : NULL;
    if (!transcript || !rail) return false;
    int left = node_index(scene, transcript), right = node_index(scene, rail);
    if (agent_ui_add_section_header(builder,left,gallery_key(203),"Conversation primitives",
            "system / user / assistant / live reasoning","A11Y READY") < 0 ||
        agent_ui_add_message(builder,left,agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE,10),
            &(agent_ui_message_model_t){AGENT_UI_MESSAGE_SYSTEM,"SYSTEM","Workspace online · 285 tools · trust policy active","14:40",false,false}) < 0 ||
        agent_ui_add_message(builder,left,agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE,11),
            &(agent_ui_message_model_t){AGENT_UI_MESSAGE_USER,"YOU","Show the exact component states an agent terminal needs.","14:41",false,true}) < 0 ||
        agent_ui_add_message(builder,left,agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE,12),
            &(agent_ui_message_model_t){AGENT_UI_MESSAGE_ASSISTANT,"DSCO","I’ll keep every state legible, actionable, and semantically addressable.","NOW",true,false}) < 0 ||
        agent_ui_add_reasoning(builder,left,agent_ui_component_key(AGENT_UI_COMPONENT_REASONING,10),
            &(agent_ui_reasoning_model_t){"Reasoning disclosure","Visible without turning chain-of-thought into visual noise.","EVALUATING",0.44f,true,true}) < 0 ||
        agent_ui_add_composer(builder,left,agent_ui_component_key(AGENT_UI_COMPONENT_COMPOSER,10),
            &(agent_ui_composer_component_model_t){"Compare compact and expanded states","Ask DSCO…","PLAN","SEND",true,false}) < 0)
        return false;
    if (agent_ui_add_section_header(builder,right,gallery_key(204),"Supporting surfaces",
            "code / commands / notices","6 TYPES") < 0 ||
        agent_ui_add_code_block(builder,right,agent_ui_component_key(AGENT_UI_COMPONENT_CODE,10),
            &(agent_ui_code_model_t){"C","agent_ui_add_message(&ui, parent, key, &message);","native_ui",true}) < 0 ||
        agent_ui_add_command(builder,right,agent_ui_component_key(AGENT_UI_COMPONENT_COMMAND,10),
            &(agent_ui_command_model_t){"Open tool activity","Inspect inputs, output, latency, and owner","⌘1",true,false}) < 0 ||
        agent_ui_add_command(builder,right,agent_ui_component_key(AGENT_UI_COMPONENT_COMMAND,11),
            &(agent_ui_command_model_t){"Toggle reasoning activity","Show or collapse execution rationale","⌘R",false,false}) < 0 ||
        agent_ui_add_command(builder,right,agent_ui_component_key(AGENT_UI_COMPONENT_COMMAND,12),
            &(agent_ui_command_model_t){"Export transcript","Write a governed session artifact","⌘E",false,true}) < 0 ||
        agent_ui_add_notification(builder,right,agent_ui_component_key(AGENT_UI_COMPONENT_NOTIFICATION,10),
            &(agent_ui_notification_model_t){"Streaming response","The live region updates without moving the composer.","PAUSE",AGENT_UI_TONE_ACCENT,false}) < 0)
        return false;
    return true;
}

static bool build_execution(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene = builder->scene;
    const agent_ui_theme_t *theme = builder->theme;
    native_ui_node_t *columns = add_columns(scene, content, gallery_key(300), theme->spacing.lg);
    native_ui_node_t *tools = columns ? add_stack(scene,node_index(scene,columns),gallery_key(301),5,theme->spacing.sm) : NULL;
    native_ui_node_t *plan = columns ? add_stack(scene,node_index(scene,columns),gallery_key(302),4,theme->spacing.sm) : NULL;
    native_ui_node_t *telemetry = columns ? add_stack(scene,node_index(scene,columns),gallery_key(303),3,theme->spacing.sm) : NULL;
    if (!tools || !plan || !telemetry) return false;
    int a=node_index(scene,tools), b=node_index(scene,plan), c=node_index(scene,telemetry);
    if (agent_ui_add_section_header(builder,a,gallery_key(304),"Tool lifecycle","queued / running / success / failure","LIVE") < 0 ||
        agent_ui_add_tool(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_TOOL,20),
            &(agent_ui_tool_model_t){"read_workspace","Index retained component contracts","42 files","00.4s","COMPLETE",AGENT_UI_TONE_SUCCESS,1.0f,false}) < 0 ||
        agent_ui_add_tool(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_TOOL,21),
            &(agent_ui_tool_model_t){"compile","Build arm64 optimized gallery","clang -O2 · CoreText · zlib","01.2s","RUNNING",AGENT_UI_TONE_ACCENT,0.61f,true}) < 0 ||
        agent_ui_add_tool(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_TOOL,22),
            &(agent_ui_tool_model_t){"runtime_probe","Verify Kitty graphics capability","awaiting terminal response","—","QUEUED",AGENT_UI_TONE_NEUTRAL,0.0f,false}) < 0)
        return false;
    if (agent_ui_add_section_header(builder,b,gallery_key(305),"Plan progression","stable steps / focus / state","3 / 5") < 0 ||
        agent_ui_add_plan_step(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP,20),
            &(agent_ui_plan_step_model_t){1,"Inspect compositor boundaries","native_ui + px_backend","DONE",AGENT_UI_TONE_SUCCESS,false,false}) < 0 ||
        agent_ui_add_plan_step(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP,21),
            &(agent_ui_plan_step_model_t){2,"Implement token system","16 complete themes","DONE",AGENT_UI_TONE_SUCCESS,false,false}) < 0 ||
        agent_ui_add_plan_step(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP,22),
            &(agent_ui_plan_step_model_t){3,"Render component gallery","exact Retina RGB","ACTIVE",AGENT_UI_TONE_ACCENT,true,true}) < 0 ||
        agent_ui_add_plan_step(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_PLAN_STEP,23),
            &(agent_ui_plan_step_model_t){4,"Run behavioral tests","semantics + layout + PPM","QUEUED",AGENT_UI_TONE_NEUTRAL,false,true}) < 0 ||
        agent_ui_add_artifact(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_ARTIFACT,20),
            &(agent_ui_artifact_model_t){"Retina gallery frame","/tmp/dsco-agent-ui-gallery.ppm","Exact RGB proof artifact","IMAGE","REVEAL",true}) < 0)
        return false;
    if (agent_ui_add_section_header(builder,c,gallery_key(306),"Execution telemetry","cost / latency / throughput","P95") < 0 ||
        agent_ui_add_metric(builder,c,agent_ui_component_key(AGENT_UI_COMPONENT_METRIC,20),
            &(agent_ui_metric_model_t){"TOKENS / SEC","186","t/s","+14%","72 83 101 116 143 186",AGENT_UI_TONE_SUCCESS}) < 0 ||
        agent_ui_add_metric(builder,c,agent_ui_component_key(AGENT_UI_COMPONENT_METRIC,21),
            &(agent_ui_metric_model_t){"TURN COST","0.084","USD","−8%","48 44 46 39 35 31",AGENT_UI_TONE_SUCCESS}) < 0 ||
        agent_ui_add_metric(builder,c,agent_ui_component_key(AGENT_UI_COMPONENT_METRIC,22),
            &(agent_ui_metric_model_t){"QUEUE","3","jobs","+1","1 2 1 3 2 3",AGENT_UI_TONE_WARNING}) < 0 ||
        agent_ui_add_timeline_event(builder,c,agent_ui_component_key(AGENT_UI_COMPONENT_TIMELINE,20),
            &(agent_ui_timeline_model_t){"14:43","Build started","7 compilation units",AGENT_UI_TONE_ACCENT}) < 0 ||
        agent_ui_add_timeline_event(builder,c,agent_ui_component_key(AGENT_UI_COMPONENT_TIMELINE,21),
            &(agent_ui_timeline_model_t){"14:44","Theme validation passed","16 / 16 readable",AGENT_UI_TONE_SUCCESS}) < 0)
        return false;
    return true;
}

static bool build_orchestration(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene=builder->scene; const agent_ui_theme_t *theme=builder->theme;
    native_ui_node_t *columns=add_columns(scene,content,gallery_key(400),theme->spacing.lg);
    native_ui_node_t *left=columns?add_stack(scene,node_index(scene,columns),gallery_key(401),4,theme->spacing.sm):NULL;
    native_ui_node_t *right=columns?add_stack(scene,node_index(scene,columns),gallery_key(402),6,theme->spacing.sm):NULL;
    if(!left||!right)return false; int a=node_index(scene,left),b=node_index(scene,right);
    if(agent_ui_add_section_header(builder,a,gallery_key(403),"Topology","ownership / routes / active work","4 AGENTS")<0||
       agent_ui_add_topology(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_TOPOLOGY,30),
            &(agent_ui_topology_model_t){"Overmind topology","orchestrator→workers","One coordinator, three bounded workstreams",4,3})<0||
       agent_ui_add_queue_item(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_QUEUE,30),
            &(agent_ui_queue_model_t){"Visual regression pass","Compare exact frame hashes","READY",1,AGENT_UI_TONE_SUCCESS,true})<0||
       agent_ui_add_queue_item(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_QUEUE,31),
            &(agent_ui_queue_model_t){"Runtime adoption","Wire selected primitives into live shell","WAITING",2,AGENT_UI_TONE_WARNING,false})<0||
       agent_ui_add_queue_item(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_QUEUE,32),
            &(agent_ui_queue_model_t){"Documentation","Publish usage and invariants","QUEUED",3,AGENT_UI_TONE_NEUTRAL,false})<0)return false;
    if(agent_ui_add_section_header(builder,b,gallery_key(404),"Agent fleet","state / model / task / context","3 ACTIVE")<0)return false;
    native_ui_node_t *grid=add_node(scene,b,gallery_key(405),NATIVE_UI_ELEMENT_GRID,NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    if(!grid)return false; grid->style.flow=NATIVE_UI_FLOW_GRID; grid->style.grid_columns=2;
    grid->style.gap=theme->spacing.md; grid->constraints.grow=1;
    int g=node_index(scene,grid);
    if(agent_ui_add_agent_card(builder,g,agent_ui_component_key(AGENT_UI_COMPONENT_AGENT,30),
        &(agent_ui_agent_model_t){"Coordinator","gpt-5.6-terra","Own component architecture","EXECUTING",NATIVE_UI_AGENT_EXECUTING,0.48f,true})<0||
       agent_ui_add_agent_card(builder,g,agent_ui_component_key(AGENT_UI_COMPONENT_AGENT,31),
        &(agent_ui_agent_model_t){"Visual QA","gpt-5.6-luna","Inspect Retina hierarchy","RESPONDING",NATIVE_UI_AGENT_RESPONDING,0.31f,false})<0||
       agent_ui_add_agent_card(builder,g,agent_ui_component_key(AGENT_UI_COMPONENT_AGENT,32),
        &(agent_ui_agent_model_t){"Test runner","local/clang","Exercise semantic contracts","EXECUTING",NATIVE_UI_AGENT_EXECUTING,0.18f,false})<0||
       agent_ui_add_agent_card(builder,g,agent_ui_component_key(AGENT_UI_COMPONENT_AGENT,33),
        &(agent_ui_agent_model_t){"Docs","gpt-5.6-mini","Await verified API","WAITING",NATIVE_UI_AGENT_WAITING,0.09f,false})<0)return false;
    return true;
}

static bool build_governance(agent_ui_builder_t *builder, int content) {
    native_ui_scene_t *scene=builder->scene; const agent_ui_theme_t *theme=builder->theme;
    native_ui_node_t *columns=add_columns(scene,content,gallery_key(500),theme->spacing.lg);
    native_ui_node_t *left=columns?add_stack(scene,node_index(scene,columns),gallery_key(501),5,theme->spacing.sm):NULL;
    native_ui_node_t *right=columns?add_stack(scene,node_index(scene,columns),gallery_key(502),4,theme->spacing.sm):NULL;
    if(!left||!right)return false; int a=node_index(scene,left),b=node_index(scene,right);
    if(agent_ui_add_section_header(builder,a,gallery_key(503),"Governed actions","permission / scope / recovery","POLICY ON")<0||
       agent_ui_add_permission(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_PERMISSION,40),
        &(agent_ui_permission_model_t){"Run a local build","Compile the new graphics modules and gallery target.","exec: make dsco-agent-ui-gallery","ALLOW ONCE","DENY",AGENT_UI_TONE_WARNING})<0||
       agent_ui_add_permission(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_PERMISSION,41),
        &(agent_ui_permission_model_t){"Publish an external artifact","This action would leave the local workspace.","net: github.com/dsco/components","REVIEW","CANCEL",AGENT_UI_TONE_DANGER})<0||
       agent_ui_add_notification(builder,a,agent_ui_component_key(AGENT_UI_COMPONENT_NOTIFICATION,40),
        &(agent_ui_notification_model_t){"Policy boundary retained","Read-only inspection remains available while approval is pending.","VIEW POLICY",AGENT_UI_TONE_SUCCESS,false})<0)return false;
    if(agent_ui_add_section_header(builder,b,gallery_key(504),"Audit surface","events / commands / evidence","IMMUTABLE")<0||
       agent_ui_add_code_block(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_CODE,40),
        &(agent_ui_code_model_t){"AUDIT","14:44:08 permission.request exec local-build\n14:44:12 operator.allow once","2 events",true})<0||
       agent_ui_add_timeline_event(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_TIMELINE,40),
        &(agent_ui_timeline_model_t){"14:44","Capability classified","exec / local workspace",AGENT_UI_TONE_WARNING})<0||
       agent_ui_add_timeline_event(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_TIMELINE,41),
        &(agent_ui_timeline_model_t){"14:44","Operator decision","allow once",AGENT_UI_TONE_SUCCESS})<0||
       agent_ui_add_command(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_COMMAND,40),
        &(agent_ui_command_model_t){"Open governance inspector","Review grants, taint, and tool scope","⌘G",true,false})<0||
       agent_ui_add_command(builder,b,agent_ui_component_key(AGENT_UI_COMPONENT_COMMAND,41),
        &(agent_ui_command_model_t){"Emergency stop","Interrupt active execution paths","⌘.",false,false})<0)return false;
    return true;
}

static bool build_themes(agent_ui_builder_t *builder, int content,
                         const agent_ui_theme_t *selected) {
    native_ui_scene_t *scene=builder->scene;
    native_ui_node_t *grid=add_node(scene,content,gallery_key(600),NATIVE_UI_ELEMENT_GRID,
                                    NATIVE_UI_ROLE_COMMAND_PALETTE);
    if(!grid)return false;
    grid->style.flow=NATIVE_UI_FLOW_GRID; grid->style.grid_columns=4;
    grid->style.gap=builder->theme->spacing.md; grid->constraints.grow=1;
    int parent=node_index(scene,grid);
    for(size_t i=0;i<agent_ui_theme_count();i++){
        const agent_ui_theme_t *theme=agent_ui_theme_at(i);
        if(agent_ui_add_theme_swatch(builder,parent,
             agent_ui_component_key(AGENT_UI_COMPONENT_THEME_SWATCH,(uint32_t)i+1),
             theme,theme==selected)<0)return false;
    }
    return true;
}

static bool build_chrome(native_ui_scene_t *scene, int width, int height,
                         agent_ui_gallery_page_t page,
                         const agent_ui_theme_t *theme,
                         int *content_index) {
    native_ui_scene_init(scene,width,height);
    native_ui_node_t *root=native_ui_scene_node(scene,scene->root);
    root->key=gallery_key(0); root->element=NATIVE_UI_ELEMENT_ROOT;
    root->role=NATIVE_UI_ROLE_AGENT_SHELL; root->style.flow=NATIVE_UI_FLOW_COLUMN;
    root->style.padding=(native_ui_insets_t){theme->spacing.lg,theme->spacing.xl,
                                            theme->spacing.md,theme->spacing.xl};
    root->style.gap=theme->spacing.sm; root->style.background=NATIVE_UI_COLOR_CANVAS;
    native_ui_node_set_accessibility_label(root,"DSCO agent UI component gallery");

    native_ui_node_t *header=add_node(scene,scene->root,gallery_key(1),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_HEADER);
    if(!header)return false; fixed_height(header,58); header->style.flow=NATIVE_UI_FLOW_ROW;
    header->style.align=NATIVE_UI_ALIGN_CENTER; header->style.gap=theme->spacing.md;
    native_ui_node_t *identity=add_stack(scene,node_index(scene,header),gallery_key(2),1,1);
    if(!identity)return false;
    native_ui_node_t *title=add_node(scene,node_index(scene,identity),gallery_key(3),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_HEADER);
    native_ui_node_t *subtitle=add_node(scene,node_index(scene,identity),gallery_key(4),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_HEADER);
    if(!title||!subtitle)return false;
    fixed_height(title,32); fixed_height(subtitle,19);
    native_ui_node_set_text(title,"DSCO / AGENT UI COMPONENT LIBRARY");
    title->style.type=NATIVE_UI_TYPE_TITLE; title->style.foreground=NATIVE_UI_COLOR_TEXT;
    native_ui_node_set_text(subtitle,"retained semantics · exact Retina pixels · Kitty transport · zero-allocation scenes");
    subtitle->style.type=NATIVE_UI_TYPE_LABEL; subtitle->style.foreground=NATIVE_UI_COLOR_TEXT_MUTED;
    native_ui_node_t *page_label=add_node(scene,node_index(scene,header),gallery_key(5),NATIVE_UI_ELEMENT_BADGE,NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *theme_label=add_node(scene,node_index(scene,header),gallery_key(6),NATIVE_UI_ELEMENT_BADGE,NATIVE_UI_ROLE_COMMAND_PALETTE);
    if(!page_label||!theme_label)return false; fixed_width(page_label,150); fixed_width(theme_label,170);
    fixed_height(page_label,28); fixed_height(theme_label,28);
    native_ui_node_set_text(page_label,agent_ui_gallery_page_name(page));
    page_label->style.type=NATIVE_UI_TYPE_LABEL; page_label->style.foreground=NATIVE_UI_COLOR_ACCENT;
    page_label->style.background=NATIVE_UI_COLOR_SURFACE; page_label->style.border=NATIVE_UI_COLOR_BORDER;
    page_label->style.border_width=1; page_label->style.radius=(uint8_t)theme->radius.pill;
    native_ui_node_set_text(theme_label,theme->name); theme_label->style.type=NATIVE_UI_TYPE_LABEL;
    theme_label->style.foreground=NATIVE_UI_COLOR_TEXT; theme_label->style.background=NATIVE_UI_COLOR_SURFACE_RAISED;
    theme_label->style.border=NATIVE_UI_COLOR_ACCENT; theme_label->style.border_width=1;
    theme_label->style.radius=(uint8_t)theme->radius.pill;

    native_ui_node_t *nav=add_node(scene,scene->root,gallery_key(7),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_COMMAND_PALETTE);
    if(!nav)return false; fixed_height(nav,38); nav->style.flow=NATIVE_UI_FLOW_ROW;
    nav->style.gap=theme->spacing.sm;
    for(int i=0;i<AGENT_UI_GALLERY_PAGE_COUNT;i++){
        bool selected=i==(int)page;
        native_ui_node_t *tab=add_node(scene,node_index(scene,nav),gallery_key(10+(unsigned)i),
                                       NATIVE_UI_ELEMENT_BADGE,NATIVE_UI_ROLE_COMMAND_PALETTE);
        if(!tab)return false; tab->constraints.grow=1; fixed_height(tab,32);
        tab->state|=NATIVE_UI_STATE_FOCUSABLE;
        if(selected)tab->state|=NATIVE_UI_STATE_SELECTED;
        char label[64]; snprintf(label,sizeof(label),"%d  %s",i+1,agent_ui_gallery_page_name((agent_ui_gallery_page_t)i));
        native_ui_node_set_text(tab,label); tab->style.type=NATIVE_UI_TYPE_LABEL;
        tab->style.foreground=selected?NATIVE_UI_COLOR_ACCENT:NATIVE_UI_COLOR_TEXT_MUTED;
        tab->style.background=selected?NATIVE_UI_COLOR_SURFACE_RAISED:NATIVE_UI_COLOR_SURFACE;
        tab->style.border=selected?NATIVE_UI_COLOR_FOCUS:NATIVE_UI_COLOR_BORDER;
        tab->style.border_width=1; tab->style.radius=(uint8_t)theme->radius.sm;
        native_ui_node_set_accessibility_label(tab,agent_ui_gallery_page_name((agent_ui_gallery_page_t)i));
    }
    native_ui_node_t *content=add_node(scene,scene->root,gallery_key(20),NATIVE_UI_ELEMENT_STACK,NATIVE_UI_ROLE_NONE);
    if(!content)return false; content->style.flow=NATIVE_UI_FLOW_COLUMN; content->constraints.grow=1;
    content->state|=NATIVE_UI_STATE_CLIPS; *content_index=node_index(scene,content);
    native_ui_node_t *footer=add_node(scene,scene->root,gallery_key(21),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_STATUS);
    if(!footer)return false; fixed_height(footer,22); footer->style.flow=NATIVE_UI_FLOW_ROW;
    native_ui_node_t *keys=add_node(scene,node_index(scene,footer),gallery_key(22),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *resolution=add_node(scene,node_index(scene,footer),gallery_key(23),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_STATUS);
    if(!keys||!resolution)return false; keys->constraints.grow=1;
    native_ui_node_set_text(keys,"1–7 PAGE  ·  ← → NAVIGATE  ·  [ ] THEME  ·  TAB NEXT THEME  ·  Q CLOSE");
    keys->style.type=NATIVE_UI_TYPE_LABEL; keys->style.foreground=NATIVE_UI_COLOR_TEXT_MUTED;
    char pixels[96]; snprintf(pixels,sizeof(pixels),"LOGICAL %d × %d  /  DEVICE SCALE OWNED BY BACKEND",width,height);
    fixed_width(resolution,350); native_ui_node_set_text(resolution,pixels);
    resolution->style.type=NATIVE_UI_TYPE_LABEL; resolution->style.foreground=NATIVE_UI_COLOR_ACCENT;
    return true;
}

bool agent_ui_gallery_build(native_ui_scene_t *scene, int logical_width,
                            int logical_height, agent_ui_gallery_page_t page,
                            const agent_ui_theme_t *theme) {
    if(!scene||logical_width<900||logical_height<700)return false;
    if(page<0||page>=AGENT_UI_GALLERY_PAGE_COUNT)page=AGENT_UI_GALLERY_WORKBENCH;
    if(!theme)theme=agent_ui_theme_default();
    int content=-1;
    if(!build_chrome(scene,logical_width,logical_height,page,theme,&content))return false;
    agent_ui_builder_t builder;
    if(!agent_ui_builder_init(&builder,scene,theme))return false;
    bool ok=false;
    switch(page){
        case AGENT_UI_GALLERY_FOUNDATIONS: ok=build_foundations(&builder,content); break;
        case AGENT_UI_GALLERY_CONVERSATION: ok=build_conversation(&builder,content); break;
        case AGENT_UI_GALLERY_EXECUTION: ok=build_execution(&builder,content); break;
        case AGENT_UI_GALLERY_ORCHESTRATION: ok=build_orchestration(&builder,content); break;
        case AGENT_UI_GALLERY_GOVERNANCE: ok=build_governance(&builder,content); break;
        case AGENT_UI_GALLERY_THEMES: ok=build_themes(&builder,content,theme); break;
        case AGENT_UI_GALLERY_WORKBENCH: default: ok=build_workbench(&builder,content); break;
    }
    if(!ok)return false;
    native_ui_layout(scene);
    return true;
}

bool agent_ui_gallery_write_ppm(const char *path, int physical_width,
                                int physical_height, int backing_scale,
                                agent_ui_gallery_page_t page,
                                const agent_ui_theme_t *theme) {
    if(!path||!*path)return false;
    agent_ui_canvas_t *canvas=agent_ui_canvas_create(physical_width,physical_height,
                                                      backing_scale,theme);
    if(!canvas)return false;
    native_ui_scene_t scene;
    bool ok=agent_ui_gallery_build(&scene,agent_ui_canvas_logical_width(canvas),
        agent_ui_canvas_logical_height(canvas),page,theme?theme:agent_ui_theme_default())&&
        agent_ui_canvas_render(canvas,&scene,NULL)&&agent_ui_canvas_write_ppm(canvas,path);
    agent_ui_canvas_destroy(canvas);
    return ok;
}

static int scale_override(void) {
    const char *value=getenv("DSCO_AGENT_UI_DPR");
    if(!value||!*value)value=getenv("DSCO_TASTE_DPR");
    if(!value||!*value)return 0;
    char *end=NULL; long scale=strtol(value,&end,10);
    return end!=value&&*end=='\0'&&scale>=1&&scale<=4?(int)scale:0;
}

static void read_geometry(agent_ui_gallery_session_t *session,FILE *out,
                          int *width,int *height,int *columns,int *rows,int *scale) {
    struct winsize window; memset(&window,0,sizeof(window));
    if(out)(void)ioctl(fileno(out),TIOCGWINSZ,&window);
    int requested=session->forced_scale>0?session->forced_scale:scale_override();
    native_ui_viewport_metrics_t metrics=native_ui_terminal_viewport(
        window.ws_col,window.ws_row,window.ws_xpixel,window.ws_ypixel,requested);
    *scale=metrics.backing_scale;
    *width=session->forced_width>0?session->forced_width:metrics.physical_width;
    *height=session->forced_height>0?session->forced_height:metrics.physical_height;
    if(*width<=0)*width=1800*(*scale);
    if(*height<=0)*height=1000*(*scale);
    *columns=window.ws_col>0?window.ws_col:180;
    *rows=window.ws_row>0?window.ws_row:55;
}

bool agent_ui_gallery_session_begin(agent_ui_gallery_session_t *session,
                                    FILE *out, int forced_width,
                                    int forced_height, int forced_scale) {
    if(!session||!out||!kitty_graphics_available(out))return false;
    memset(session,0,sizeof(*session));
    session->forced_width=forced_width; session->forced_height=forced_height;
    session->forced_scale=forced_scale;
    session->image_id=UINT32_C(0x41474900)^((uint32_t)getpid()<<5);
    if(!session->image_id)session->image_id=1;
    fprintf(out,"\033[?1049h\033[2J\033[H\033[?25l"); fflush(out);
    session->active=!ferror(out);
    return session->active;
}

bool agent_ui_gallery_session_present(agent_ui_gallery_session_t *session,
                                      FILE *out,
                                      agent_ui_gallery_page_t page,
                                      const agent_ui_theme_t *theme) {
    if(!session||!session->active||!out)return false;
    int width,height,columns,rows,scale;
    read_geometry(session,out,&width,&height,&columns,&rows,&scale);
    agent_ui_canvas_t *canvas=agent_ui_canvas_create(width,height,scale,theme);
    if(!canvas)return false;
    native_ui_scene_t scene;
    bool ok=agent_ui_gallery_build(&scene,agent_ui_canvas_logical_width(canvas),
        agent_ui_canvas_logical_height(canvas),page,theme?theme:agent_ui_theme_default())&&
        agent_ui_canvas_render(canvas,&scene,NULL);
    if(ok){
        if(session->physical_width>0)
            fprintf(out,"\033_Ga=d,d=I,i=%u,q=2\033\\",session->image_id);
        char control[256];
        snprintf(control,sizeof(control),"a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
                 width,height,session->image_id);
        kitty_graphics_send_options_t options; kitty_graphics_send_options_default(&options);
        ok=kitty_graphics_send_pixels(out,control,agent_ui_canvas_pixels(canvas),
                                       agent_ui_canvas_size(canvas),&options);
        if(ok){
            fprintf(out,"\0337\033[H\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=1,q=2\033\\\0338",
                    session->image_id,columns,rows); fflush(out); ok=!ferror(out);
        }
    }
    agent_ui_canvas_destroy(canvas);
    if(ok){session->physical_width=width;session->physical_height=height;
           session->columns=columns;session->rows=rows;session->backing_scale=scale;}
    return ok;
}

bool agent_ui_gallery_session_geometry_changed(agent_ui_gallery_session_t *session,
                                               FILE *out) {
    if(!session||!session->active)return false;
    int width,height,columns,rows,scale;
    read_geometry(session,out,&width,&height,&columns,&rows,&scale);
    return width!=session->physical_width||height!=session->physical_height||
           columns!=session->columns||rows!=session->rows||scale!=session->backing_scale;
}

void agent_ui_gallery_session_end(agent_ui_gallery_session_t *session,
                                  FILE *out) {
    if(!session||!out||!session->active)return;
    fprintf(out,"\033_Ga=d,d=I,i=%u,q=2\033\\\033[?25h\033[?1049l",session->image_id);
    fflush(out); session->active=false;
}
