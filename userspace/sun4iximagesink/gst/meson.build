# SPDX-License-Identifier: LGPL-2.1-or-later
gst_dep = dependency('gstreamer-1.0', version : '>= 1.20')
gst_base_dep = dependency('gstreamer-base-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_alloc_dep = dependency('gstreamer-allocators-1.0')
drm_dep = dependency('libdrm')
x11_dep = dependency('x11')
x11_xcb_dep = dependency('x11-xcb')
xcb_dri2_dep = dependency('xcb-dri2')

sun4iximagesink_sources = [
  'gstsun4iximagesink.c',
  'gstsun4idrm.c',
  'gstsun4ix11.c',
]

shared_library('gstsun4iximagesink',
  sun4iximagesink_sources,
  c_args : ['-DPACKAGE="sun4iximagesink"'],
  dependencies : [gst_dep, gst_base_dep, gst_video_dep, gst_alloc_dep, drm_dep, x11_dep, x11_xcb_dep, xcb_dri2_dep],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)
