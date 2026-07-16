#include "px_widgets.h"

#include <stdio.h>
#include <string.h>

static native_ui_node_t *add_node(native_ui_scene_t *scene, int parent,
                                  uint64_t key, native_ui_element_t element,
                                  native_ui_role_t role) {
    int index = native_ui_scene_add(scene, parent, key, element, role);
    return index >= 0 ? native_ui_scene_node(scene, index) : NULL;
}

static int index_of(const native_ui_scene_t *scene,
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

static void set_text(native_ui_node_t *node, const char *text,
                     native_ui_type_token_t type,
                     native_ui_color_token_t color) {
    if (!node) return;
    native_ui_node_set_text(node, text ? text : "");
    node->style.type = type;
    node->style.foreground = color;
}

static int text_width(const char *text, int minimum, int maximum) {
    int width = (int)strlen(text ? text : "") * 7 + 18;
    if (width < minimum) width = minimum;
    if (maximum > 0 && width > maximum) width = maximum;
    return width;
}

native_ui_color_token_t px_widget_tone_token(px_widget_tone_t tone) {
    switch (tone) {
        case PX_WIDGET_TONE_ACCENT: return NATIVE_UI_COLOR_ACCENT;
        case PX_WIDGET_TONE_SUCCESS: return NATIVE_UI_COLOR_SUCCESS;
        case PX_WIDGET_TONE_WARNING: return NATIVE_UI_COLOR_WARNING;
        case PX_WIDGET_TONE_DANGER: return NATIVE_UI_COLOR_DANGER;
        case PX_WIDGET_TONE_NEUTRAL: default: return NATIVE_UI_COLOR_TEXT_MUTED;
    }
}

int px_widget_card(native_ui_scene_t *scene, int parent, uint64_t key,
                   const char *title) {
    native_ui_node_t *card = add_node(scene, parent, key,
                                      NATIVE_UI_ELEMENT_SURFACE,
                                      NATIVE_UI_ROLE_NONE);
    if (!card) return -1;
    card->style.flow = NATIVE_UI_FLOW_COLUMN;
    card->style.padding = (native_ui_insets_t){10,10,10,10};
    card->style.gap = 6;
    card->style.background = NATIVE_UI_COLOR_SURFACE;
    card->style.border = NATIVE_UI_COLOR_BORDER;
    card->style.border_width = 1;
    card->style.radius = 8;
    card->state |= NATIVE_UI_STATE_CLIPS;
    if (title && *title) {
        native_ui_node_t *heading = add_node(
            scene, index_of(scene, card), px_widget_subkey(key, 1),
            NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_HEADER);
        if (!heading) return -1;
        fixed_height(heading, 24);
        set_text(heading, title, NATIVE_UI_TYPE_TITLE, NATIVE_UI_COLOR_TEXT);
        native_ui_node_set_accessibility_label(heading, title);
    }
    native_ui_node_set_accessibility_label(card,
        title && *title ? title : "Pixel component card");
    return index_of(scene, card);
}

int px_widget_kv_row(native_ui_scene_t *scene, int parent, uint64_t key,
                     const char *label, const char *value) {
    native_ui_node_t *row = add_node(scene, parent, key, NATIVE_UI_ELEMENT_ROW,
                                     NATIVE_UI_ROLE_STATUS);
    if (!row) return -1;
    fixed_height(row, 26);
    row->style.flow = NATIVE_UI_FLOW_ROW;
    row->style.align = NATIVE_UI_ALIGN_CENTER;
    row->style.gap = 8;
    native_ui_node_t *name = add_node(scene, index_of(scene,row),
        px_widget_subkey(key,1), NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *data = add_node(scene, index_of(scene,row),
        px_widget_subkey(key,2), NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    if (!name || !data) return -1;
    name->constraints.grow = 1;
    set_text(name,label,NATIVE_UI_TYPE_LABEL,NATIVE_UI_COLOR_TEXT_MUTED);
    fixed_width(data,text_width(value,42,180));
    set_text(data,value,NATIVE_UI_TYPE_BODY,NATIVE_UI_COLOR_TEXT);
    char a11y[NATIVE_UI_LABEL_CAP];
    snprintf(a11y,sizeof(a11y),"%s: %s",label?label:"Value",value?value:"");
    native_ui_node_set_accessibility_label(row,a11y);
    return index_of(scene,row);
}

int px_widget_badge(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *text, px_widget_tone_t tone) {
    native_ui_node_t *badge = add_node(scene,parent,key,NATIVE_UI_ELEMENT_BADGE,
                                       NATIVE_UI_ROLE_STATUS);
    if(!badge)return -1;
    fixed_width(badge,text_width(text,42,140)); fixed_height(badge,22);
    set_text(badge,text,NATIVE_UI_TYPE_LABEL,
             tone==PX_WIDGET_TONE_NEUTRAL?NATIVE_UI_COLOR_TEXT:px_widget_tone_token(tone));
    badge->style.background=NATIVE_UI_COLOR_SURFACE_RAISED;
    badge->style.border=px_widget_tone_token(tone); badge->style.border_width=1;
    badge->style.radius=11; native_ui_node_set_accessibility_label(badge,text);
    return index_of(scene,badge);
}

int px_widget_meter(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *label, double value, px_widget_tone_t tone) {
    native_ui_node_t *root=add_node(scene,parent,key,NATIVE_UI_ELEMENT_STACK,
                                    NATIVE_UI_ROLE_STATUS);
    if(!root)return -1; fixed_height(root,42); root->style.flow=NATIVE_UI_FLOW_COLUMN;
    root->style.gap=2;
    native_ui_node_t *caption=add_node(scene,index_of(scene,root),px_widget_subkey(key,1),
        NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *meter=add_node(scene,index_of(scene,root),px_widget_subkey(key,2),
        NATIVE_UI_ELEMENT_METER,NATIVE_UI_ROLE_STATUS);
    if(!caption||!meter)return -1; fixed_height(caption,18); fixed_height(meter,16);
    set_text(caption,label,NATIVE_UI_TYPE_LABEL,NATIVE_UI_COLOR_TEXT_MUTED);
    meter->value=(float)(value<0?0:value>1?1:value);
    meter->style.foreground=px_widget_tone_token(tone);
    native_ui_node_set_accessibility_label(root,label);
    return index_of(scene,root);
}

int px_widget_sparkline(native_ui_scene_t *scene, int parent, uint64_t key,
                        const double *values, int count,
                        px_widget_tone_t tone) {
    if(count<0)count=0; if(count>48)count=48;
    native_ui_node_t *spark=add_node(scene,parent,key,NATIVE_UI_ELEMENT_SPARKLINE,
                                     NATIVE_UI_ROLE_TIMELINE);
    if(!spark)return -1; fixed_height(spark,42);
    char payload[NATIVE_UI_TEXT_CAP]; size_t used=0; payload[0]='\0';
    for(int i=0;i<count;i++){
        int wrote=snprintf(payload+used,sizeof(payload)-used,"%s%.4g",i?" ":"",values?values[i]:0.0);
        if(wrote<0||(size_t)wrote>=sizeof(payload)-used)break; used+=(size_t)wrote;
    }
    native_ui_node_set_text(spark,payload); spark->style.foreground=px_widget_tone_token(tone);
    native_ui_node_set_accessibility_label(spark,"Trend sparkline");
    return index_of(scene,spark);
}

int px_widget_progress(native_ui_scene_t *scene, int parent, uint64_t key,
                       double fraction, double phase, px_widget_tone_t tone) {
    native_ui_node_t *meter=add_node(scene,parent,key,NATIVE_UI_ELEMENT_METER,
                                     NATIVE_UI_ROLE_STATUS);
    if(!meter)return -1; fixed_height(meter,18);
    meter->style.foreground=px_widget_tone_token(tone);
    if(fraction<0){
        if(phase<0)phase=0;if(phase>1)phase=1;
        meter->value=(float)(-1.0-phase); meter->state|=NATIVE_UI_STATE_LIVE;
        native_ui_node_set_accessibility_label(meter,"Indeterminate progress");
    }else{
        if(fraction>1)fraction=1; meter->value=(float)fraction;
        native_ui_node_set_accessibility_label(meter,"Progress");
    }
    return index_of(scene,meter);
}

int px_widget_gauge(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *label, double value, px_widget_tone_t tone) {
    native_ui_node_t *node=add_node(scene,parent,key,NATIVE_UI_ELEMENT_CUSTOM,
                                    NATIVE_UI_ROLE_STATUS);
    if(!node)return -1; fixed_height(node,96);
    if(value<0)value=0;if(value>1)value=1;node->value=(float)value;
    node->style.foreground=px_widget_tone_token(tone);
    char tag[NATIVE_UI_TEXT_CAP];snprintf(tag,sizeof(tag),"%s:%s",PX_WIDGET_KIND_GAUGE,label?label:"");
    native_ui_node_set_text(node,tag);native_ui_node_set_accessibility_label(node,label?label:"Gauge");
    return index_of(scene,node);
}

int px_widget_bar_chart(native_ui_scene_t *scene, int parent, uint64_t key,
                        const double *values, int count,
                        px_widget_tone_t tone) {
    if(count<0)count=0;if(count>32)count=32;
    native_ui_node_t *node=add_node(scene,parent,key,NATIVE_UI_ELEMENT_CUSTOM,
                                    NATIVE_UI_ROLE_STATUS);
    if(!node)return -1;fixed_height(node,80);node->style.foreground=px_widget_tone_token(tone);
    char payload[NATIVE_UI_TEXT_CAP];size_t used=(size_t)snprintf(payload,sizeof(payload),"%s:",PX_WIDGET_KIND_BARS);
    for(int i=0;i<count&&used<sizeof(payload);i++){
        int wrote=snprintf(payload+used,sizeof(payload)-used,"%s%.4g",i?" ":"",values?values[i]:0.0);
        if(wrote<0||(size_t)wrote>=sizeof(payload)-used)break;used+=(size_t)wrote;
    }
    native_ui_node_set_text(node,payload);native_ui_node_set_accessibility_label(node,"Bar chart");
    return index_of(scene,node);
}

static int liveness_widget(native_ui_scene_t *scene,int parent,uint64_t key,
                           const char *kind,double phase,px_widget_tone_t tone,
                           const char *label) {
    native_ui_node_t *node=add_node(scene,parent,key,NATIVE_UI_ELEMENT_CUSTOM,
                                    NATIVE_UI_ROLE_REASONING_ACTIVITY);
    if(!node)return -1;fixed_height(node,28);if(phase<0)phase=0;if(phase>1)phase=1;
    node->value=(float)phase;node->style.foreground=px_widget_tone_token(tone);
    native_ui_node_set_text(node,kind);node->state|=NATIVE_UI_STATE_LIVE;
    native_ui_node_set_accessibility_label(node,label);return index_of(scene,node);
}

int px_widget_spinner(native_ui_scene_t *scene, int parent, uint64_t key,
                      double phase, px_widget_tone_t tone) {
    return liveness_widget(scene,parent,key,PX_WIDGET_KIND_SPINNER,phase,tone,"Agent activity spinner");
}

int px_widget_activity_dots(native_ui_scene_t *scene, int parent,
                            uint64_t key, double phase,
                            px_widget_tone_t tone) {
    return liveness_widget(scene,parent,key,PX_WIDGET_KIND_DOTS,phase,tone,"Agent reasoning activity");
}

static const char *state_name(native_ui_agent_state_t state) {
    return native_ui_agent_state_name(state);
}

int px_widget_agent_card(native_ui_scene_t *scene, int parent, uint64_t key,
                         const px_widget_agent_t *agent) {
    if(!agent)return -1;
    int root=px_widget_card(scene,parent,key,NULL);native_ui_node_t *card=native_ui_scene_node(scene,root);
    if(!card)return -1;fixed_height(card,132);card->role=NATIVE_UI_ROLE_AGENT_TOPOLOGY;
    card->agent_state=agent->state;card->state|=NATIVE_UI_STATE_FOCUSABLE;
    native_ui_node_t *header=add_node(scene,root,px_widget_subkey(key,1),NATIVE_UI_ELEMENT_ROW,NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    if(!header)return -1;fixed_height(header,24);header->style.flow=NATIVE_UI_FLOW_ROW;header->style.align=NATIVE_UI_ALIGN_CENTER;
    native_ui_node_t *name=add_node(scene,index_of(scene,header),px_widget_subkey(key,2),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    if(!name)return -1;name->constraints.grow=1;set_text(name,agent->name,NATIVE_UI_TYPE_TITLE,NATIVE_UI_COLOR_TEXT);
    px_widget_tone_t tone=agent->state==NATIVE_UI_AGENT_ERROR||agent->state==NATIVE_UI_AGENT_BLOCKED?PX_WIDGET_TONE_DANGER:
        agent->state==NATIVE_UI_AGENT_IDLE?PX_WIDGET_TONE_NEUTRAL:PX_WIDGET_TONE_SUCCESS;
    if(px_widget_badge(scene,index_of(scene,header),px_widget_subkey(key,3),state_name(agent->state),tone)<0)return -1;
    native_ui_node_t *task=add_node(scene,root,px_widget_subkey(key,4),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    native_ui_node_t *model=add_node(scene,root,px_widget_subkey(key,5),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_STATUS);
    if(!task||!model)return -1;fixed_height(task,22);fixed_height(model,18);
    set_text(task,agent->task,NATIVE_UI_TYPE_BODY,NATIVE_UI_COLOR_TEXT_MUTED);
    set_text(model,agent->model,NATIVE_UI_TYPE_LABEL,NATIVE_UI_COLOR_ACCENT);
    if(agent->progress>=0&&px_widget_progress(scene,root,px_widget_subkey(key,6),agent->progress,0,PX_WIDGET_TONE_ACCENT)<0)return -1;
    if(agent->cost_usd>=0){
        char cost[48];snprintf(cost,sizeof(cost),"$%.4f",agent->cost_usd);
        native_ui_node_t *cost_node=add_node(scene,root,px_widget_subkey(key,7),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_STATUS);
        if(!cost_node)return -1;fixed_height(cost_node,16);set_text(cost_node,cost,NATIVE_UI_TYPE_LABEL,NATIVE_UI_COLOR_TEXT_MUTED);
    }
    native_ui_node_set_accessibility_label(card,agent->name?agent->name:"Agent");return root;
}

int px_widget_toast(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *text, px_widget_tone_t tone) {
    native_ui_node_t *toast=add_node(scene,parent,key,NATIVE_UI_ELEMENT_SURFACE,NATIVE_UI_ROLE_TOAST);
    if(!toast)return -1;fixed_height(toast,52);toast->style.flow=NATIVE_UI_FLOW_ROW;
    toast->style.align=NATIVE_UI_ALIGN_CENTER;toast->style.padding=(native_ui_insets_t){7,10,7,7};toast->style.gap=9;
    toast->style.background=NATIVE_UI_COLOR_SURFACE_RAISED;toast->style.border=NATIVE_UI_COLOR_BORDER;
    toast->style.border_width=1;toast->style.radius=8;toast->state|=NATIVE_UI_STATE_LIVE;
    native_ui_node_t *accent=add_node(scene,index_of(scene,toast),px_widget_subkey(key,1),NATIVE_UI_ELEMENT_RULE,NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *label=add_node(scene,index_of(scene,toast),px_widget_subkey(key,2),NATIVE_UI_ELEMENT_TEXT,NATIVE_UI_ROLE_TOAST);
    if(!accent||!label)return -1;fixed_width(accent,3);accent->style.background=px_widget_tone_token(tone);
    label->constraints.grow=1;set_text(label,text,NATIVE_UI_TYPE_BODY,NATIVE_UI_COLOR_TEXT);
    native_ui_node_set_accessibility_label(toast,text);return index_of(scene,toast);
}
