# --- header --- Sections: header (imports, threads, clip = video_in, TensorRT prologue), body (this stage),
# footer (set_output). Video Player runs this file whole for one stage; for upscale + interpolation it writes ONE
# chain script: the upscale header, the upscale body, the interpolation body, then the footer.
import vapoursynth as vs
from vapoursynth import core
import k7sfunc as k7f

core.num_threads = k7f.vs_t_dft
clip = video_in
# --- body ---
ret = k7f.FPS_CTRL(clip, fps_in=container_fps, fps_ret=True)
# H_PRE (FMT_CTRL h_max, only ever shrinks) is resolved by the host from interpolation.preHeight:
# auto -> 0 (no shrink) after a VapourSynth upscaler (the upscale body above in a chain script, or a separate
# upscale filter), which already caps its output to H_MAX; an explicit value shrinks the upscaled clip;
# auto without an upscaler -> the display cap (upscaleParams.maxHeight or the monitor height).
# PlayKit order: upscale -> shrink to display size -> interpolate. 0/720/1080/1440 are explicit.
if __H_PRE__ > 0:
    clip = k7f.FMT_CTRL(clip, h_max=__H_PRE__, fmt_pix=0)
clip = k7f.DRBA_DML(
    clip,
    model=__MODEL__,
    turbo=__TURBO__,
    fps_in=container_fps,
    fps_num=__FPS_NUM__,
    fps_den=__FPS_DEN__,
    sc_mode=__SC_MODE__,
    gpu=__GPU__,
    gpu_t=__THREADS__,
)
# --- footer ---
clip.set_output()
