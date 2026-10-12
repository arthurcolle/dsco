/* Retain rasterized window CONTENT across pure translations. Input edits,
 * selection, result generation, size, focus, DPI and theme invalidate exactly.
 * Four entries, <=16 MiB conservatively charged at four bytes/physical pixel.
 * This is a raster cache, not a cap on the existing canvas pool/process RSS. */
#define WINDOW_CACHE_SLOTS 4
#define WINDOW_CACHE_LIMIT (16u*1024u*1024u)
typedef struct {
    native_window_t window;
    double scale;
    unsigned char theme[sizeof(*px_theme_active())];
    double point_size;
    px_color_t accent;
    bool focused,keyboard_focus;
} window_raster_key_t;
typedef struct { bool used; window_raster_key_t key; px_canvas_t *canvas; size_t charged_bytes; uint64_t age; } window_raster_slot_t;
static window_raster_slot_t window_rasters[WINDOW_CACHE_SLOTS];
static pthread_mutex_t window_raster_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t window_raster_age,window_raster_hits,window_raster_misses;
static size_t window_raster_bytes;
static window_raster_key_t window_raster_key(const native_window_t *w,double scale,uint64_t focused,bool keyboard,px_color_t accent) {
    window_raster_key_t key;memset(&key,0,sizeof(key));key.window=*w;
    key.window.rect.x=key.window.rect.y=0;
    key.window.restore_rect.x=key.window.restore_rect.y=0;
    key.window.revision=0;
    key.scale=scale;memcpy(key.theme,px_theme_active(),sizeof(key.theme));key.point_size=text_point_size(1);key.accent=accent;
    key.focused=w->id==focused;key.keyboard_focus=keyboard;return key;
}
static void window_raster_drop(int i) {
    if(!window_rasters[i].used)return;
    window_raster_bytes-=window_rasters[i].charged_bytes;
    free_canvas(window_rasters[i].canvas);memset(&window_rasters[i],0,sizeof(window_rasters[i]));
}
static void window_raster_clear(void) {
    pthread_mutex_lock(&window_raster_mu);
    for(int i=0;i<WINDOW_CACHE_SLOTS;i++)window_raster_drop(i);
    pthread_mutex_unlock(&window_raster_mu);
}
void pixel_tui_window_cache_stats(uint64_t *hits,uint64_t *misses,size_t *bytes) {
    pthread_mutex_lock(&window_raster_mu);
    if(hits)*hits=window_raster_hits;if(misses)*misses=window_raster_misses;if(bytes)*bytes=window_raster_bytes;
    pthread_mutex_unlock(&window_raster_mu);
}
static bool window_raster_draw(px_canvas_t *dst,const window_raster_key_t *key,native_ui_rect_t rect,native_ui_rect_t area) {
    const char *enabled=getenv("DSCO_NATIVE_WINDOW_CACHE");
    if(enabled && !strcmp(enabled,"0"))return false;
    pthread_mutex_lock(&window_raster_mu);
    for(int i=0;i<WINDOW_CACHE_SLOTS;i++)if(window_rasters[i].used && !memcmp(&window_rasters[i].key,key,sizeof(*key))) {
        window_rasters[i].age=++window_raster_age;window_raster_hits++;
        session_window_composite(dst,window_rasters[i].canvas,rect.x,rect.y,area);
        pthread_mutex_unlock(&window_raster_mu);return true;
    }
    window_raster_misses++;pthread_mutex_unlock(&window_raster_mu);return false;
}
static bool window_raster_keep(const window_raster_key_t *key,px_canvas_t *canvas) {
    const char *enabled=getenv("DSCO_NATIVE_WINDOW_CACHE");
    if(enabled && !strcmp(enabled,"0"))return false;
    if(canvas->pixel_width<=0 || canvas->pixel_height<=0 || (size_t)canvas->pixel_width>WINDOW_CACHE_LIMIT/4/(size_t)canvas->pixel_height)return false;
    size_t bytes=(size_t)canvas->pixel_width*canvas->pixel_height*4;
    pthread_mutex_lock(&window_raster_mu);
    /* Replace an older raster for the same semantic window before evicting others. */
    for(int i=0;i<WINDOW_CACHE_SLOTS;i++)if(window_rasters[i].used && window_rasters[i].key.window.id==key->window.id)window_raster_drop(i);
    for(;;) {
        int free_slot=-1,oldest=-1;
        for(int i=0;i<WINDOW_CACHE_SLOTS;i++) {
            if(!window_rasters[i].used)free_slot=i;
            else if(oldest<0 || window_rasters[i].age<window_rasters[oldest].age)oldest=i;
        }
        if(free_slot>=0 && window_raster_bytes+bytes<=WINDOW_CACHE_LIMIT) {
            window_rasters[free_slot]=(window_raster_slot_t){.used=true,.key=*key,.canvas=canvas,.charged_bytes=bytes,.age=++window_raster_age};
            window_raster_bytes+=bytes;pthread_mutex_unlock(&window_raster_mu);return true;
        }
        if(oldest<0){pthread_mutex_unlock(&window_raster_mu);return false;}
        window_raster_drop(oldest);
    }
}
