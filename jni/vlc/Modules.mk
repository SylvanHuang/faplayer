# modules begin
LOCAL_STATIC_LIBRARIES += access_http_plugin access_mms_plugin amem_plugin android_surface_plugin audiotrack_android_plugin avcodec_plugin avformat_plugin bandlimited_resampler_plugin blend_plugin converter_fixed_plugin dummy_plugin filesystem_plugin fixed32_mixer_plugin float32_mixer_plugin freetype_plugin libasf_plugin libass_plugin libavi_plugin libmp4_plugin live555_plugin mkv_plugin mpeg_audio_plugin mpgv_plugin packetizer_copy_plugin packetizer_dirac_plugin packetizer_flac_plugin packetizer_h264_plugin packetizer_mlp_plugin packetizer_mpeg4audio_plugin packetizer_mpeg4video_plugin packetizer_mpegvideo_plugin packetizer_vc1_plugin realrtsp_plugin simple_channel_mixer_plugin subsdec_plugin subsusf_plugin subtitle_plugin swscale_plugin trivial_mixer_plugin ugly_resampler_plugin vmem_plugin yuv2rgb_plugin
# modules end

LOCAL_STATIC_LIBRARIES += libass libfreetype libiconv libcharset liblive555 libebml libmatroska

# it's the only choice
ifeq ($(APP_STL),gnustl_static)
LOCAL_STATIC_LIBRARIES += gnustl_static
endif

LOCAL_WHOLE_STATIC_LIBRARIES += libavformat libavfilter libavcodec libavdevice libavutil libswscale

