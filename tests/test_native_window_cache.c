#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
int g_cheap_mode;vm_t g_vm;volatile int g_interrupted;double g_cost_budget;
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);}}while(0)
int main(void){
 native_window_t w={0};w.id=11;w.kind=NATIVE_WINDOW_TASK;w.status=NATIVE_WINDOW_READY;w.rect=(native_ui_rect_t){20,40,310,240};w.restore_rect=w.rect;w.editor_active=true;
 snprintf(w.title,sizeof(w.title),"Local JSON");snprintf(w.factory,sizeof(w.factory),"json");snprintf(w.text,sizeof(w.text),"{\"pi\":3.14,\"unicode\":\"π\"}\nline two");
 native_buffer_editor_reset(&w.editor,strlen(w.text));
 native_ui_rect_t area={0,0,620,440};
 for(int si=0;si<3;si++){
  double scale=si==0?1:si==1?1.5:2;
  px_canvas_t *reference=canvas_acquire_scaled(620,440,scale),*cached=canvas_acquire_scaled(620,440,scale);CHECK(reference&&cached);
  for(int variant=0;variant<9;variant++){
   w.rect.x=20+variant*7;w.rect.y=40+variant*3;w.revision++;
   if(variant==3){w.task_show_result=true;w.editor_active=false;w.status=NATIVE_WINDOW_DONE;snprintf(w.task_output,sizeof(w.task_output),"{\"ok\":true,\"values\":6}");}
   if(variant==5){w.task_show_result=false;w.editor_active=true;strcat(w.text,"\nnew input");w.editor.anchor=0;w.editor.cursor=5;}
   if(variant==7)w.rect.width=350;
   fill_rect(reference,0,0,620,440,C_PANEL_ALT,1.0);fill_rect(cached,0,0,620,440,C_PANEL_ALT,1.0);
   setenv("DSCO_NATIVE_WINDOW_CACHE","0",1);draw_session_window(reference,&w,w.id,true,area,C_GREEN);
   setenv("DSCO_NATIVE_WINDOW_CACHE","1",1);draw_session_window(cached,&w,w.id,true,area,C_GREEN);
   size_t bytes=(size_t)reference->pixel_width*reference->pixel_height*3;
   CHECK(!memcmp(reference->pixels,cached->pixels,bytes));
  }
  free_canvas(reference);free_canvas(cached);
 }
 uint64_t hits=0,misses=0;size_t bytes=0;pixel_tui_window_cache_stats(&hits,&misses,&bytes);CHECK(hits>0);CHECK(misses>0);CHECK(bytes<=WINDOW_CACHE_LIMIT);
 window_raster_key_t key=window_raster_key(&w,1,w.id,true,C_GREEN);px_canvas_t oversized={.pixel_width=100000,.pixel_height=100000};CHECK(!window_raster_keep(&key,&oversized));
 window_raster_clear();pixel_tui_window_cache_stats(NULL,NULL,&bytes);CHECK(bytes==0);
 native_buffer_editor_dispose(&w.editor);printf("native_window_cache: %d checks, %d failures; cached/uncached pixel parity at 3 scales\n",checks,failures);return failures?1:0;
}
