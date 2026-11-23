"""Windows choices for Pod Programs' authenticated Display page."""
import os
from pathlib import Path
from tempfile import NamedTemporaryFile
from urllib.error import HTTPError
from urllib.request import Request as ControlRequest, urlopen

from fastapi import APIRouter, Form, Request

WINDOW_MODES = {"windowed": "Windowed", "borderless": "Borderless fullscreen", "exclusive": "Exclusive fullscreen"}


def install(control, token):
    # desktop_runtime configures the Pod data directory before importing page modules.
    import android_display as display
    import bunker_text

    display.DEFAULTS.update(fps="144", idle="off")
    display.FPS = [(str(rate), f"{rate} FPS") for rate in (30, 60, 90, 120, 144, 165)] + [
        ("max", "pods.display.option.fps_max")]
    display.RESOLUTIONS = [
        ("default", "1600 × 900"), *[(str(edge), f"{edge}p") for edge in (540, 640, 720, 900, 1080, 1440, 2160)],
        ("native", "pods.display.option.res_native")]
    available = Path(__file__).with_name("Fsr31.dll").is_file() and Path(__file__).with_name("amd_fidelityfx_vk.dll").is_file()
    display.UPSCALE = [("off", "pods.display.option.upscale_off")]
    if available:
        display.UPSCALE.append(("fsr31", "AMD FidelityFX Super Resolution 3.1"))
    router = APIRouter()

    def settings():
        result = display.read_settings()
        path = display.conf_path()
        try:
            stored = dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
        except FileNotFoundError:
            stored = {}
        mode = stored.get("window_mode")
        result["window_mode"] = mode if mode in WINDOW_MODES else "windowed"
        for key, default, maximum in (("fsr_quality", 1, 4), ("fsr_sharpness", 20, 100)):
            try:
                value = int(stored.get(key, default))
            except ValueError:
                value = default
            result[key] = value if 0 <= value <= maximum else default
        return result

    @router.get("/display")
    def view(request: Request, message: str = "", error: str = ""):
        return display.templates.TemplateResponse(request, "desktop-display.html", {
            "active": "display", "settings": settings(), "fps": display.FPS, "upscalers": display.UPSCALE,
            "window_modes": WINDOW_MODES.items(), "resolutions": display.RESOLUTIONS, "textures": display.TEXTURES, "idle": display.IDLE,
            "effects": display.EFFECT, "message": message, "error": error})

    @router.post("/display")
    def save(fps: str = Form(...), resolution: str = Form(...), textures: str = Form("full"),
             idle: str = Form("off"), shadows: str = Form("on"), reflections: str = Form("on"), bloom: str = Form("on"),
             upscale: str = Form("off"), fsr_quality: int = Form(1, ge=0, le=4), fsr_sharpness: int = Form(20, ge=0, le=100),
             window_mode: str = Form(None)):
        previous = settings()
        if window_mode is None:
            window_mode = previous["window_mode"]
        if window_mode not in WINDOW_MODES:
            return display.redirect(error="Choose a valid window mode.")
        path = display.conf_path()
        stage = None
        try:
            with NamedTemporaryFile(dir=path.parent, delete=False) as temp:
                stage = Path(temp.name)
            display.write_settings(fps, resolution, "full", path=stage, textures=textures, idle=idle,
                                   shadows=shadows, reflections=reflections, bloom=bloom, upscale=upscale)
            with stage.open("a") as target:
                target.write(f"fsr_quality={fsr_quality}\nfsr_sharpness={fsr_sharpness}\n")
                target.write(f"window_mode={window_mode}\n")
            os.replace(stage, path)
        except (ValueError, OSError) as exc:
            return display.redirect(error=str(exc))
        finally:
            if stage is not None:
                stage.unlink(missing_ok=True)
                stage.with_name(stage.name+".tmp").unlink(missing_ok=True)
        if control:
            resize = int(previous["resolution"] != resolution)
            request = ControlRequest(f"{control}/display?resize={resize}", data=b"", headers={"X-Bunker-Token": token})
            try:
                with urlopen(request, timeout=10):
                    pass
            except HTTPError as exc:
                return display.redirect(error=exc.read(512).decode(errors="replace").strip())
            except OSError:
                return display.redirect(error="Display settings saved for the next launch. The player did not respond.")
        return display.redirect(message=bunker_text.t("pods.runtime.saved", next=bunker_text.t("pods.runtime.deploy")))

    display.router = router
