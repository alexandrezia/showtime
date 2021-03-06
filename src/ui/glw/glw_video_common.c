/*
 *  Video output on GL surfaces
 *  Copyright (C) 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>

#include "showtime.h"
#include "media.h"
#include "video/video_playback.h"
#include "video/video_settings.h"

#include "glw.h"
#include "glw_video_common.h"
#include "glw_video_overlay.h"
#include "glw_renderer.h"
#include "glw_texture.h"

static glw_video_engine_t glw_video_blank;


static void glw_video_input(const frame_info_t *info, void *opaque);


/**
 *
 */
static void
glw_video_rctx_adjust(glw_rctx_t *rc, const glw_video_t *gv)
{
  const glw_root_t *gr = gv->w.glw_root;

  if(gr->gr_underscan_h || gr->gr_underscan_v) {
    glw_reposition(rc,
		   -gr->gr_underscan_h,
		   rc->rc_height + gr->gr_underscan_v,
		   rc->rc_width  + gr->gr_underscan_h,
		   -gr->gr_underscan_v);
  }

  float t_aspect = (float)gv->gv_dar_num / gv->gv_dar_den; 

  if(gv->gv_fstretch)
    return;

  if(t_aspect * rc->rc_height < rc->rc_width) {

    if(gv->gv_hstretch)
      return;

    // Shrink X
    int border = rc->rc_width - t_aspect * rc->rc_height;
    int left  = (border + 1) / 2;
    int right = rc->rc_width - (border / 2);

    float s = (right - left) / (float)rc->rc_width;
    float t = -1.0f + (right + left) / (float)rc->rc_width;

    glw_Translatef(rc, t, 0, 0);
    glw_Scalef(rc, s, 1.0f, 1.0f);

    rc->rc_width = right - left;

  } else {
    // Shrink Y
    int border = rc->rc_height - rc->rc_width / t_aspect;
    int bottom  = (border + 1) / 2;
    int top     = rc->rc_height - (border / 2);

    float s = (top - bottom) / (float)rc->rc_height;
    float t = -1.0f + (top + bottom) / (float)rc->rc_height;

    glw_Translatef(rc, 0, t, 0);
    glw_Scalef(rc, 1.0f, s, 1.0f);
    rc->rc_height = top - bottom;
  }
}


/**
 *
 */
static int
glw_video_widget_event(event_t *e, media_pipe_t *mp, int in_menu)
{
  if(event_is_action(e, ACTION_PLAYPAUSE) ||
     event_is_action(e, ACTION_PLAY) ||
     event_is_action(e, ACTION_PAUSE) ||
     event_is_action(e, ACTION_ACTIVATE)) {
    mp_enqueue_event(mp, e);
    return 1;
  }

  if(event_is_action(e, ACTION_UP) ||
     event_is_action(e, ACTION_DOWN) ||
     event_is_action(e, ACTION_LEFT) ||
     event_is_action(e, ACTION_RIGHT)) {
    
    if(in_menu) {
      mp_enqueue_event(mp, e);
      return 1;
    }

    if(event_is_action(e, ACTION_LEFT)) {
      e = event_create_action(ACTION_SEEK_BACKWARD);
      mp_enqueue_event(mp, e);
      event_release(e);
      return 1;
    }

    if(event_is_action(e, ACTION_RIGHT)) {
      e = event_create_action(ACTION_SEEK_FORWARD);
      mp_enqueue_event(mp, e);
      event_release(e);
      return 1;
    }

    if(event_is_action(e, ACTION_UP)) {
      e = event_create_action(ACTION_VOLUME_UP);
      event_dispatch(e);
      return 1;
    }

    if(event_is_action(e, ACTION_DOWN)) {
      e = event_create_action(ACTION_VOLUME_DOWN);
      event_dispatch(e);
      return 1;
    }
  }
  return 0;
}


/**
 *
 */
int
glw_video_compute_output_duration(video_decoder_t *vd, int frame_duration)
{
  int delta;
  const int maxdiff = 5000;

  if(vd->vd_avdiff_x > 0) {
    delta = pow(vd->vd_avdiff_x * 1000.0f, 2);
    if(delta > maxdiff)
      delta = maxdiff;

  } else if(vd->vd_avdiff_x < 0) {
    delta = -pow(-vd->vd_avdiff_x * 1000.0f, 2);
    if(delta < -maxdiff)
      delta = -maxdiff;
  } else {
    delta = 0;
  }
  return frame_duration + delta;
}


/**
 *
 */
void
glw_video_compute_avdiff(glw_root_t *gr, video_decoder_t *vd, media_pipe_t *mp, 
			 int64_t pts, int epoch)
{
  int64_t aclock;
  const char *status;

  if(mp->mp_audio_clock_epoch != epoch) {
    /* Not the same clock epoch, can not sync */
    vd->vd_avdiff_x = 0;
    kalman_init(&vd->vd_avfilter);
    if(mp->mp_stats)
      prop_set_int(mp->mp_prop_avdiff_error, 3);
    return;
  }

  if(vd->vd_compensate_thres > 0) {
    vd->vd_compensate_thres--;
    vd->vd_avdiff_x = 0;
    kalman_init(&vd->vd_avfilter);
    if(mp->mp_stats)
      prop_set_int(mp->mp_prop_avdiff_error, 2);
    return;
  }
  

  hts_mutex_lock(&mp->mp_clock_mutex);

  aclock = mp->mp_audio_clock + gr->gr_frame_start_avtime
    - mp->mp_audio_clock_avtime;

  hts_mutex_unlock(&mp->mp_clock_mutex);

  aclock += mp->mp_avdelta;

  vd->vd_avdiff = aclock - (pts - 16666) - vd->vd_avd_delta;

  if(abs(vd->vd_avdiff) < 10000000) {

    vd->vd_avdiff_x = kalman_update(&vd->vd_avfilter, 
				    (double)vd->vd_avdiff / 1000000);
    if(vd->vd_avdiff_x > 10.0f)
      vd->vd_avdiff_x = 10.0f;
    
    if(vd->vd_avdiff_x < -10.0f)
      vd->vd_avdiff_x = -10.0f;
    if(mp->mp_stats)
      prop_set_int(mp->mp_prop_avdiff_error, 0);
    status = "lock";
  } else {
    status = "nolock";
    if(mp->mp_stats)
      prop_set_int(mp->mp_prop_avdiff_error, 1);
  }

  if(mp->mp_stats) {
    if(!vd->vd_may_update_avdiff) {
      prop_set_float(mp->mp_prop_avdiff, vd->vd_avdiff_x);
      vd->vd_may_update_avdiff = 5;
    } else {
      vd->vd_may_update_avdiff--;
    }
  }

  if(0) {
    static int64_t lastpts, lastaclock, lastclock;
   
   TRACE(TRACE_DEBUG, "AVDIFF", "%10f %10d %15"PRId64":a:%-8"PRId64" %15"PRId64":v:%-8"PRId64" %15"PRId64" %15"PRId64" %s %lld", 
	 vd->vd_avdiff_x,
	 vd->vd_avdiff,
	 aclock,
	 aclock - lastaclock,
	 pts,
	 pts - lastpts,
	 mp->mp_audio_clock,
	 gr->gr_frame_start_avtime - lastclock,
	 status,
	 showtime_get_avtime() - aclock);
  lastpts = pts;
  lastaclock = aclock;
  lastclock = gr->gr_frame_start_avtime;
 }
}




/**
 *
 */
static void
glw_video_play(glw_video_t *gv)
{
  event_t *e;
  
  if(gv->gv_freezed)
    return;

  if(!strcmp(gv->gv_current_url ?: "", gv->gv_pending_url ?: ""))
    return;

  mystrset(&gv->gv_current_url, gv->gv_pending_url ?: "");

  e = event_create_playurl(gv->gv_current_url, 
			   !!(gv->gv_flags & GLW_VIDEO_PRIMARY),
			   gv->gv_priority,
			   !!(gv->gv_flags & GLW_VIDEO_NO_AUDIO),
			   gv->gv_model,
			   gv->gv_how);
  mp_enqueue_event(gv->gv_mp, e);
  event_release(e);
}


/**
 * We are going away, flush out all surfaces
 * and destroy zombie video decoder 
 */
static void
glw_video_dtor(glw_t *w)
{
  glw_video_t *gv = (glw_video_t *)w;
  video_decoder_t *vd = gv->gv_vd;

  prop_ref_dec(gv->gv_model);
  prop_unsubscribe(gv->gv_vo_scaling_sub);
  prop_unsubscribe(gv->gv_vzoom_sub);
  prop_unsubscribe(gv->gv_hstretch_sub);
  prop_unsubscribe(gv->gv_fstretch_sub);
  prop_unsubscribe(gv->gv_vo_on_video_sub);

  free(gv->gv_current_url);
  free(gv->gv_pending_url);
  free(gv->gv_how);

  glw_video_overlay_deinit(gv);
  
  LIST_REMOVE(gv, gv_global_link);
  video_decoder_destroy(vd);

  glw_video_surfaces_cleanup(gv);

  hts_cond_destroy(&gv->gv_avail_queue_cond);
  hts_cond_destroy(&gv->gv_reconf_cond);
  hts_mutex_destroy(&gv->gv_surface_mutex);

  mp_ref_dec(gv->gv_mp);
  gv->gv_mp = NULL;
}


/**
 *
 */
static void
glw_video_newframe(glw_t *w, int flags)
{
  glw_video_t *gv = (glw_video_t *)w;
  video_decoder_t *vd = gv->gv_vd;
  int64_t pts;

  hts_mutex_lock(&gv->gv_surface_mutex);

  if(memcmp(&gv->gv_cfg_cur, &gv->gv_cfg_req, sizeof(glw_video_config_t)))
    glw_video_surface_reconfigure(gv);

  pts = gv->gv_cfg_cur.gvc_engine->gve_newframe(gv, vd, flags);

  hts_mutex_unlock(&gv->gv_surface_mutex);

  if(pts != PTS_UNSET)
    glw_video_overlay_set_pts(gv, pts);
}


/**
 *
 */
static int 
glw_video_widget_callback(glw_t *w, void *opaque, glw_signal_t signal, 
			  void *extra)
{
  glw_video_t *gv = (glw_video_t *)w;
  video_decoder_t *vd = gv->gv_vd;
  glw_rctx_t *rc, rc0;

  switch(signal) {
  case GLW_SIGNAL_LAYOUT:

    rc = extra;
    rc0 = *rc;
    glw_video_rctx_adjust(&rc0, gv);
    glw_video_overlay_layout(gv, rc, &rc0);
    return 0;

  case GLW_SIGNAL_EVENT:
    return glw_video_widget_event(extra, gv->gv_mp, gv->gv_spu_in_menu);

  case GLW_SIGNAL_DESTROY:
    hts_mutex_lock(&gv->gv_surface_mutex);
    hts_cond_signal(&gv->gv_reconf_cond);
    hts_cond_signal(&gv->gv_avail_queue_cond);
    hts_mutex_unlock(&gv->gv_surface_mutex);
    video_playback_destroy(gv->gv_mp);
    video_decoder_stop(vd);
    return 0;

  case GLW_SIGNAL_POINTER_EVENT:
    return glw_video_overlay_pointer_event(vd, 
					   gv->gv_cfg_cur.gvc_width[0],
					   gv->gv_cfg_cur.gvc_height[0],
					   extra, gv->gv_mp);

  default:
    return 0;
  }
}


/**
 *
 */
static void
glw_video_ctor(glw_t *w)
{
  glw_video_t *gv = (glw_video_t *)w;
  glw_root_t *gr = w->glw_root;


  gv->gv_cfg_req.gvc_engine = &glw_video_blank;
  gv->gv_cfg_cur.gvc_engine = &glw_video_blank;

  TAILQ_INIT(&gv->gv_avail_queue);
  TAILQ_INIT(&gv->gv_displaying_queue);
  TAILQ_INIT(&gv->gv_decoded_queue);

  hts_mutex_init(&gv->gv_surface_mutex);
  hts_cond_init(&gv->gv_avail_queue_cond, &gv->gv_surface_mutex);
  hts_cond_init(&gv->gv_reconf_cond, &gv->gv_surface_mutex);

  gv->gv_mp = mp_create("Video decoder", MP_VIDEO | MP_PRIMABLE, NULL);
#if CONFIG_GLW_BACKEND_OPENGL
  if(video_settings.vdpau)
    gv->gv_mp->mp_vdpau_dev = gr->gr_be.gbr_vdpau_dev;
#endif

  LIST_INSERT_HEAD(&gr->gr_video_decoders, gv, gv_global_link);

  gv->gv_mp->mp_video_frame_deliver = glw_video_input;
  gv->gv_mp->mp_video_frame_opaque = gv;

  gv->gv_vd = video_decoder_create(gv->gv_mp);
  video_playback_create(gv->gv_mp);

  gv->gv_vo_scaling_sub =
    prop_subscribe(0,
		   PROP_TAG_SET_FLOAT, &gv->gv_vo_scaling,
		   PROP_TAG_COURIER, w->glw_root->gr_courier,
		   PROP_TAG_ROOT,
		   settings_get_value(gv->gv_mp->mp_setting_sub_scale),
		   NULL);

  gv->gv_vzoom_sub =
    prop_subscribe(0,
		   PROP_TAG_SET_INT, &gv->gv_vzoom,
		   PROP_TAG_COURIER, w->glw_root->gr_courier,
		   PROP_TAG_ROOT,
		   settings_get_value(gv->gv_mp->mp_setting_vzoom),
		   NULL);

  gv->gv_hstretch_sub =
    prop_subscribe(0,
		   PROP_TAG_SET_INT, &gv->gv_hstretch,
		   PROP_TAG_COURIER, w->glw_root->gr_courier,
		   PROP_TAG_ROOT,
		   settings_get_value(gv->gv_mp->mp_setting_hstretch),
		   NULL);


  gv->gv_fstretch_sub =
    prop_subscribe(0,
		   PROP_TAG_SET_INT, &gv->gv_fstretch,
		   PROP_TAG_COURIER, w->glw_root->gr_courier,
		   PROP_TAG_ROOT,
		   settings_get_value(gv->gv_mp->mp_setting_fstretch),
		   NULL);

  gv->gv_vo_on_video_sub =
    prop_subscribe(0,
		   PROP_TAG_SET_INT, &gv->gv_vo_on_video,
		   PROP_TAG_COURIER, w->glw_root->gr_courier,
		   PROP_TAG_ROOT,
		   settings_get_value(gv->gv_mp->mp_setting_sub_on_video),
		   NULL);
}


/**
 *
 */
static void 
mod_video_flags(glw_t *w, int set, int clr)
{
  glw_video_t *gv = (glw_video_t *)w;
  gv->gv_flags = (gv->gv_flags | set) & ~clr;
}


/**
 *
 */
static void
set_how(glw_t *w, const char *how)
{
  glw_video_t *gv = (glw_video_t *)w;

  if(how == NULL)
    return;
  
  mystrset(&gv->gv_how, how);
  glw_video_play(gv);
}

/**
 *
 */
static void
set_source(glw_t *w, rstr_t *url)
{
  glw_video_t *gv = (glw_video_t *)w;

  if(url == NULL)
    return;
  
  mystrset(&gv->gv_pending_url, rstr_get(url));
  glw_video_play(gv);
}


/**
 *
 */
static void
freeze(glw_t *w)
{
  glw_video_t *gv = (glw_video_t *)w;
  gv->gv_freezed = 1;
}


/**
 *
 */
static void
thaw(glw_t *w)
{
  glw_video_t *gv = (glw_video_t *)w;
  gv->gv_freezed = 0;
  glw_video_play(gv);
}


/**
 *
 */
static void
glw_video_set(glw_t *w, va_list ap)
{
  glw_video_t *gv = (glw_video_t *)w;
  glw_attribute_t attrib;
  prop_t *p, *p2;
  event_t *e;

  do {
    attrib = va_arg(ap, int);
    switch(attrib) {

    case GLW_ATTRIB_PROPROOTS3:
      p = va_arg(ap, void *);
      assert(p != NULL);
      p2 = prop_create(p, "media");
      
      prop_link(gv->gv_mp->mp_prop_root, p2);
      
      (void)va_arg(ap, void *); // Parent, just throw it away
      (void)va_arg(ap, void *); // Clone, just throw it away
      break;

    case GLW_ATTRIB_PRIORITY:
      gv->gv_priority = va_arg(ap, int);

      e = event_create_int(EVENT_PLAYBACK_PRIORITY, gv->gv_priority);
      mp_enqueue_event(gv->gv_mp, e);
      event_release(e);
      break;

    case GLW_ATTRIB_PROP_MODEL:
      if(gv->gv_model)
	prop_ref_dec(gv->gv_model);

      gv->gv_model = prop_ref_inc(va_arg(ap, prop_t *));
      break;

    default:
      GLW_ATTRIB_CHEW(attrib, ap);
      break;
    }
  } while(attrib);
}




/**
 *
 */
void 
glw_video_render(glw_t *w, const glw_rctx_t *rc)
{
  glw_video_t *gv = (glw_video_t *)w;
  glw_rctx_t rc0 = *rc;

  glw_video_rctx_adjust(&rc0, gv);

  gv->gv_rwidth  = rc0.rc_width;
  gv->gv_rheight = rc0.rc_height;

  if(glw_is_focusable(w))
    glw_store_matrix(w, &rc0);

  glw_rctx_t rc1 = rc0;

  if(gv->gv_vzoom != 100) {
    float zoom = gv->gv_vzoom / 100.0f;
    glw_Scalef(&rc1, zoom, zoom, 1.0);
  }

  gv->gv_cfg_cur.gvc_engine->gve_render(gv, &rc1);
  glw_video_overlay_render(gv, rc, &rc0);
}


/**
 *
 */
static glw_class_t glw_video = {
  .gc_name = "video",
  .gc_instance_size = sizeof(glw_video_t),
  .gc_set = glw_video_set,
  .gc_ctor = glw_video_ctor,
  .gc_dtor = glw_video_dtor,
  .gc_render = glw_video_render,
  .gc_newframe = glw_video_newframe,
  .gc_signal_handler = glw_video_widget_callback,
  .gc_mod_video_flags = mod_video_flags,
  .gc_set_source = set_source,
  .gc_set_how = set_how,
  .gc_freeze = freeze,
  .gc_thaw = thaw,
};

GLW_REGISTER_CLASS(glw_video);


/**
 *
 */
int
glw_video_configure(glw_video_t *gv,
		    const glw_video_engine_t *engine,
		    const int *wvec, const int *hvec,
		    int surfaces, int flags, int pixfmt)
{
  glw_video_config_t gvc = {0};

  assert(surfaces <= GLW_VIDEO_MAX_SURFACES);

  gvc.gvc_valid = 1;

  gvc.gvc_engine = engine;

  if(wvec != NULL) {
    gvc.gvc_width[0] = wvec[0];
    gvc.gvc_width[1] = wvec[1];
    gvc.gvc_width[2] = wvec[2];
  }

  if(hvec != NULL) {
    gvc.gvc_height[0] = hvec[0];
    gvc.gvc_height[1] = hvec[1];
    gvc.gvc_height[2] = hvec[2];
  }

  gvc.gvc_nsurfaces = surfaces;
  gvc.gvc_flags = flags;
  gvc.gvc_pixfmt = pixfmt;

  if(memcmp(&gvc, &gv->gv_cfg_cur, sizeof(gvc))) {

    memcpy(&gv->gv_cfg_req, &gvc, sizeof(gvc));
    while(memcmp(&gvc, &gv->gv_cfg_cur, sizeof(gvc))) {
    
      if(gv->w.glw_flags & GLW_DESTROYING)
	return -1;

      hts_cond_wait(&gv->gv_reconf_cond, &gv->gv_surface_mutex);
    }
  }

  // Reset screen saver every time we get a new frame
  gv->w.glw_root->gr_screensaver_counter = 0;
  return 0;
}
	       

/**
 *
 */
glw_video_surface_t *
glw_video_get_surface(glw_video_t *gv)
{
  glw_video_surface_t *s;

  while((s = TAILQ_FIRST(&gv->gv_avail_queue)) == NULL) {
    if(gv->w.glw_flags & GLW_DESTROYING)
      return NULL;

    hts_cond_wait(&gv->gv_avail_queue_cond, &gv->gv_surface_mutex);
  }

  TAILQ_REMOVE(&gv->gv_avail_queue, s, gvs_link);
  hts_mutex_assert(&gv->gv_surface_mutex);
  return s;
}


/**
 *
 */
void
glw_video_put_surface(glw_video_t *gv, glw_video_surface_t *s,
		      int64_t pts, int epoch, int duration, int yshift)
{
  s->gvs_pts = pts;
  s->gvs_epoch = epoch;
  s->gvs_duration = duration;
  s->gvs_yshift = yshift;
  TAILQ_INSERT_TAIL(&gv->gv_decoded_queue, s, gvs_link);
  hts_mutex_assert(&gv->gv_surface_mutex);
}


static LIST_HEAD(, glw_video_engine) engines;

void
glw_register_video_engine(glw_video_engine_t *gve)
{
  LIST_INSERT_HEAD(&engines, gve, gve_link);
}


/**
 * Frame delivery from video decoder
 */
static void 
glw_video_input(const frame_info_t *fi, void *opaque)
{
  glw_video_t *gv = opaque;
  glw_video_engine_t *gve;

  hts_mutex_lock(&gv->gv_surface_mutex);

  if(fi == NULL) {
    // Blackout
    glw_video_configure(gv, &glw_video_blank, NULL, NULL, 0, 0, 0);
  } else {

    gv->gv_dar_num = fi->fi_dar_num;
    gv->gv_dar_den = fi->fi_dar_den;
    gv->gv_vheight = fi->fi_height;
  
    LIST_FOREACH(gve, &engines, gve_link)
      if(gve->gve_type == fi->fi_type)
	gve->gve_deliver(fi, gv);
  }

  hts_mutex_unlock(&gv->gv_surface_mutex);
}



/**
 *
 */
void
glw_video_surfaces_cleanup(glw_video_t *gv)
{
  gv->gv_cfg_cur.gvc_engine->gve_reset(gv);
  TAILQ_INIT(&gv->gv_avail_queue);
  TAILQ_INIT(&gv->gv_displaying_queue);
  TAILQ_INIT(&gv->gv_decoded_queue);
}





/**
 *
 */
void
glw_video_reset(glw_root_t *gr)
{
  glw_video_t *gv;

  LIST_FOREACH(gv, &gr->gr_video_decoders, gv_global_link) {

    hts_mutex_lock(&gv->gv_surface_mutex);

    glw_video_surfaces_cleanup(gv);
    gv->gv_cfg_cur.gvc_engine = &glw_video_blank;

    hts_mutex_unlock(&gv->gv_surface_mutex);
  }
}



/**
 * 
 */
void
glw_video_surface_reconfigure(glw_video_t *gv)
{
  glw_video_surfaces_cleanup(gv);

  gv->gv_cfg_cur = gv->gv_cfg_req;
  gv->gv_cfg_cur.gvc_engine->gve_init(gv);

  hts_cond_signal(&gv->gv_reconf_cond);
}


/**
 *
 */
static int64_t
blank_newframe(glw_video_t *gv, video_decoder_t *vd, int flags)
{
  return PTS_UNSET;
}

/**
 *
 */
static void
blank_render(glw_video_t *gv, glw_rctx_t *rc)
{
}


/**
 *
 */
static void
blank_reset(glw_video_t *gv)
{
}

/**
 *
 */
static int
blank_init(glw_video_t *gv)
{
  return 0;
}

/**
 *
 */
static glw_video_engine_t glw_video_blank = {
  .gve_newframe = blank_newframe,
  .gve_render = blank_render,
  .gve_reset = blank_reset,
  .gve_init = blank_init,
};




#if ENABLE_LIBAV


static void
video_deliver_lavc(const frame_info_t *fi, glw_video_t *gv)
{
  frame_info_t nfi = *fi;

  switch(fi->fi_pix_fmt) {
  case PIX_FMT_YUV420P:
  case PIX_FMT_YUV422P:
  case PIX_FMT_YUV444P:
  case PIX_FMT_YUV410P:
  case PIX_FMT_YUV411P:
  case PIX_FMT_YUV440P:
  case PIX_FMT_YUVJ420P:
  case PIX_FMT_YUVJ422P:
  case PIX_FMT_YUVJ444P:
  case PIX_FMT_YUVJ440P:
    avcodec_get_chroma_sub_sample(nfi.fi_pix_fmt, &nfi.fi_hshift,
				  &nfi.fi_vshift);
    
    nfi.fi_type = 'YUVP';
    break;
    
  case PIX_FMT_VDPAU_H264:
  case PIX_FMT_VDPAU_MPEG1:
  case PIX_FMT_VDPAU_MPEG2:
  case PIX_FMT_VDPAU_WMV3:
  case PIX_FMT_VDPAU_VC1:
  case PIX_FMT_VDPAU_MPEG4:
    nfi.fi_type = 'VDPA';
    break;
    
  default:
    return;
  }
  glw_video_engine_t *gve;

  LIST_FOREACH(gve, &engines, gve_link)
    if(gve->gve_type == nfi.fi_type)
      gve->gve_deliver(&nfi, gv);
}


/**
 *
 */
static glw_video_engine_t glw_video_lavc = {
  .gve_type = 'LAVC',
  .gve_deliver = video_deliver_lavc,
};

GLW_REGISTER_GVE(glw_video_lavc);

#endif

