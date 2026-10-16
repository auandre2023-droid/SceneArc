# --- header --- Sections: header (imports, threads, clip = video_in, TensorRT prologue), body (this stage),
# footer (set_output). Video Player runs this file whole for one stage; for upscale + interpolation it writes ONE
# chain script: the upscale header, the upscale body, the interpolation body, then the footer.
import vapoursynth as vs
from vapoursynth import core
import k7sfunc as k7f

# Diagnostics prologue (Video Player): appends to data/logs/vs-trt.log (path from the host) which vs-mlrt / TensorRT build
# and GPU this script runs on, then wraps vsmlrt.trtexec (resolved as a module global at call time by
# vsmlrt._inference) to record the engine chosen, cache hit or build, build seconds, and the trtexec log on failure.
# Best effort only: every step is guarded, so a logging problem never stops the script.
def _vp_trt_prologue(log_path, template, gpu):
    import os
    import re
    import time

    # The generated file name; the dunder name is built at run time because the host rejects a script that still
    # contains two underscores in a row after substituting its placeholders.
    script = os.path.basename(str(globals().get("_" * 2 + "file" + "_" * 2, "") or ""))
    tag = template + ((" " + script) if script else "")

    def text(value):
        if isinstance(value, bytes):
            return value.decode("utf-8", "replace")
        return str(value)

    def write(line):
        try:
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(time.strftime("%Y-%m-%d %H:%M:%S") + " [" + tag + "] " + line + "\n")
        except Exception:
            pass

    try:
        version = core.trt.Version()
        parts = ["vs-mlrt=" + text(version.get("version", "?")),
                 "tensorrt=" + text(version.get("tensorrt_version", "?")),
                 "cuda_runtime=" + text(version.get("cuda_runtime_version", "?")),
                 "plugin=" + text(version.get("path", "?"))]
        try:
            props = core.trt.DeviceProperties(gpu)
            total = props.get("total_global_memory", 0)
            parts += ["gpu=" + str(gpu), "name=" + text(props.get("name", "?")),
                      "total_mem_mib=" + str(int(total) // (1024 * 1024) if total else "?")]
        except Exception as error:
            parts += ["gpu=" + str(gpu), "device_properties_error=" + repr(error)]
        write("runtime " + " ".join(parts))
    except Exception as error:
        write("runtime query failed: " + repr(error))

    try:
        from k7sfunc._external import vsmlrt as vm
        original = getattr(vm.trtexec, "_vp_original", vm.trtexec)

        def trtexec(network_path, *args, **kwargs):
            started = time.time()
            try:
                engine = original(network_path, *args, **kwargs)
            except Exception as error:
                message = text(error)
                found = re.search(r"written to (.+?\.log)", message)
                log_hint = found.group(1) if found else os.path.join(os.environ.get("TEMP", "%TEMP%"), "trtexec_*.log")
                first = message.splitlines()[0][:300] if message else repr(error)
                write("engine build FAILED onnx=" + text(network_path) + " seconds=%.1f" % (time.time() - started) +
                      " error=" + first + " trtexec_log=" + log_hint)
                raise
            try:
                # Cache hit: the engine file is older than this call. A TensorRT build takes far longer than a
                # second, so a sub-second return is a hit too (guards against coarse file timestamps).
                cached = os.path.getmtime(engine) < started or time.time() - started < 1.0
                write("engine " + ("cache-hit" if cached else "built") + " seconds=%.1f" % (time.time() - started) +
                      " path=" + text(engine) + " onnx=" + text(network_path))
            except Exception:
                pass
            return engine

        trtexec._vp_original = original
        vm.trtexec = trtexec
    except Exception as error:
        write("trtexec wrapper not installed: " + repr(error))


try:
    _vp_trt_prologue(__TRT_LOG__, "fi_drba_trt.vpy", __GPU__)
except Exception:
    pass

core.num_threads = k7f.vs_t_dft
clip = video_in
# --- body ---
# TensorRT backend (NVIDIA only), parameters from mpv_PlayKit's MEMC_DRBA_NV.vpy (int8_qnt=False, ws_size=0).
# turbo is the TensorRT DRBA setting: 1 (PlayKit default, drba/<model>_ap.onnx) or 2 (half-resolution flow,
# drba/<model>_scale_ap.onnx); no _fp16 under TensorRT. Both are static engines: every distinct input resolution
# builds its own engine on first use (minutes), and mpv reloads the script (and the engine) on every seek (seconds).
ret = k7f.FPS_CTRL(clip, fps_in=container_fps, fps_ret=True)
# H_PRE (FMT_CTRL h_max, only ever shrinks) is resolved by the host from interpolation.preHeight:
# auto -> 0 (no shrink) after a VapourSynth upscaler (the upscale body above in a chain script, or a separate
# upscale filter), which already caps its output to H_MAX; an explicit value shrinks the upscaled clip;
# auto without an upscaler -> the display cap (upscaleParams.maxHeight or the monitor height).
# PlayKit order: upscale -> shrink to display size -> interpolate. 0/720/1080/1440 are explicit.
if __H_PRE__ > 0:
    clip = k7f.FMT_CTRL(clip, h_max=__H_PRE__, fmt_pix=0)
clip = k7f.DRBA_NV(
    clip,
    model=__MODEL__,
    int8_qnt=False,
    turbo=__TURBO__,
    fps_in=container_fps,
    fps_num=__FPS_NUM__,
    fps_den=__FPS_DEN__,
    sc_mode=__SC_MODE__,
    gpu=__GPU__,
    gpu_t=__THREADS__,
    ws_size=0,
)
# --- footer ---
clip.set_output()
