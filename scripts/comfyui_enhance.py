"""Enhances a folder of pictures with an image-editing model in ComfyUI (FLUX.2 [klein] 4B, distilled): each picture
goes in with an instruction ("make it sharper and more detailed, change nothing else") and the edited picture comes
out under the same name, at a whole multiple of the input's size. It is the upscaler for psptex enhance:

    psptex enhance assets/textures assets/textures-ai --skip-layer assets/textures-hd
        --upscaler "python scripts/comfyui_enhance.py {in} {out}"

Uses ComfyUI Desktop's installation (its Python, models and custom nodes), starting a server of its own without a
window on another port (the Desktop app may run alongside), and stops it at the end. Needs in ComfyUI's models:
diffusion_models/flux-2-klein-4b-fp8.safetensors, text_encoders/qwen_3_4b.safetensors, vae/flux2-vae.safetensors.

    python scripts/comfyui_enhance.py --info        # the parameters of the nodes it uses, as installed
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

DESKTOP = Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/ComfyUI/resources/ComfyUI'
BASE = Path.home() / 'Documents/ComfyUI'
MODELS_CONFIG = Path(os.environ.get('APPDATA', '')) / 'ComfyUI/extra_models_config.yaml'
NODES = ['UNETLoader', 'CLIPLoader', 'VAELoader', 'LoadImage', 'ImageScaleToTotalPixels', 'GetImageSize',
         'EmptyFlux2LatentImage', 'Flux2Scheduler', 'CLIPTextEncode', 'ConditioningZeroOut', 'VAEEncode',
         'ReferenceLatent', 'CFGGuider', 'RandomNoise', 'KSamplerSelect', 'SamplerCustomAdvanced', 'VAEDecode',
         'SaveImage']
# The owner's prompt (it worked best with Gemini), plus a line against what this model adds on its own: asking for
# "sharp and highly detailed" gave crunchy textures, and "keep the original's look exactly" gave little more than a
# blurred original; this one gives clean new detail (rock, metal, panel lines), and psptex enhance pulls the colours
# back to the original's (2026-10-16 comparison).
PROMPT = ('Upscale this texture from Final Fantasy VII Crisis Core using AI enhancement, while preserving its original '
          'aspect ratio and style. Smooth areas stay smooth: do not add cracks, scratches, stains or dirt that are not '
          'in the original.')


def request(url, data=None, headers=None):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    with urllib.request.urlopen(req, timeout=600) as r:
        return r.read()


def get_json(server, path):
    return json.loads(request(server + path))


class Server:
    """A ComfyUI server: the one at --server, or one of our own (started, and stopped on exit)."""

    def __init__(self, args, temp):
        self.url = args.server or 'http://127.0.0.1:%d' % args.port
        self.process = self.log = None
        if self.alive():
            return
        if args.server:
            sys.exit('no ComfyUI server at ' + args.server)
        python = BASE / '.venv/Scripts/python.exe'
        if not python.exists() or not (DESKTOP / 'main.py').exists():
            sys.exit('ComfyUI Desktop not found (%s, %s)' % (python, DESKTOP))
        self.io = Path(temp)
        (self.io / 'in').mkdir(parents=True, exist_ok=True)
        (self.io / 'out').mkdir(parents=True, exist_ok=True)
        self.log = log = open(self.io / 'server.log', 'w', encoding='utf-8', errors='replace')
        cmd = [str(python), str(DESKTOP / 'main.py'), '--base-directory', str(BASE),
               '--front-end-root', str(DESKTOP / 'web_custom_versions/desktop_app'),
               '--input-directory', str(self.io / 'in'), '--output-directory', str(self.io / 'out'),
               '--listen', '127.0.0.1', '--port', str(args.port), '--disable-auto-launch']
        if MODELS_CONFIG.exists():
            cmd += ['--extra-model-paths-config', str(MODELS_CONFIG)]
        print('starting ComfyUI on port %d (log: %s)' % (args.port, self.io / 'server.log'), file=sys.stderr)
        self.process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        for _ in range(300):
            if self.alive():
                return
            if self.process.poll() is not None:
                sys.exit('ComfyUI exited (exit code %s); see %s' % (self.process.returncode, self.io / 'server.log'))
            time.sleep(1)
        self.stop()
        sys.exit('ComfyUI did not start in 5 minutes; see %s' % (self.io / 'server.log'))

    def alive(self):
        try:
            get_json(self.url, '/system_stats')
            return True
        except Exception:
            return False

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(30)
        if self.log:
            self.log.close()


def upload(server, path):
    boundary = uuid.uuid4().hex
    body = b''.join([
        b'--' + boundary.encode() + b'\r\n',
        b'Content-Disposition: form-data; name="image"; filename="' + path.name.encode() + b'"\r\n',
        b'Content-Type: image/png\r\n\r\n', path.read_bytes(), b'\r\n',
        b'--' + boundary.encode() + b'\r\n',
        b'Content-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n',
        b'--' + boundary.encode() + b'--\r\n'])
    reply = json.loads(request(server + '/upload/image', body,
                               {'Content-Type': 'multipart/form-data; boundary=' + boundary}))
    return reply['name']


def graph(image, prompt, seed, megapixels, args):
    """The template's single-picture edit (image_flux2_klein_image_edit_4b_distilled), as an API prompt."""
    return {
        'unet': {'class_type': 'UNETLoader', 'inputs': {'unet_name': args.unet, 'weight_dtype': 'default'}},
        'clip': {'class_type': 'CLIPLoader', 'inputs': {'clip_name': args.clip, 'type': 'flux2', 'device': 'default'}},
        'vae': {'class_type': 'VAELoader', 'inputs': {'vae_name': args.vae}},
        'load': {'class_type': 'LoadImage', 'inputs': {'image': image}},
        'scale': {'class_type': 'ImageScaleToTotalPixels',
                  'inputs': {'image': ['load', 0], 'upscale_method': args.prescale, 'megapixels': megapixels,
                             'resolution_steps': 16}},
        'size': {'class_type': 'GetImageSize', 'inputs': {'image': ['scale', 0]}},
        'latent': {'class_type': 'EmptyFlux2LatentImage',
                   'inputs': {'width': ['size', 0], 'height': ['size', 1], 'batch_size': 1}},
        'sigmas': {'class_type': 'Flux2Scheduler',
                   'inputs': {'steps': args.steps, 'width': ['size', 0], 'height': ['size', 1]}},
        'text': {'class_type': 'CLIPTextEncode', 'inputs': {'clip': ['clip', 0], 'text': prompt}},
        'zero': {'class_type': 'ConditioningZeroOut', 'inputs': {'conditioning': ['text', 0]}},
        'ref': {'class_type': 'VAEEncode', 'inputs': {'pixels': ['scale', 0], 'vae': ['vae', 0]}},
        'positive': {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['text', 0], 'latent': ['ref', 0]}},
        'negative': {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['zero', 0], 'latent': ['ref', 0]}},
        'guider': {'class_type': 'CFGGuider',
                   'inputs': {'model': ['unet', 0], 'positive': ['positive', 0], 'negative': ['negative', 0], 'cfg': 1.0}},
        'noise': {'class_type': 'RandomNoise', 'inputs': {'noise_seed': seed}},
        'sampler': {'class_type': 'KSamplerSelect', 'inputs': {'sampler_name': 'euler'}},
        'sample': {'class_type': 'SamplerCustomAdvanced',
                   'inputs': {'noise': ['noise', 0], 'guider': ['guider', 0], 'sampler': ['sampler', 0],
                              'sigmas': ['sigmas', 0], 'latent_image': ['latent', 0]}},
        'decode': {'class_type': 'VAEDecode', 'inputs': {'samples': ['sample', 0], 'vae': ['vae', 0]}},
        'save': {'class_type': 'SaveImage', 'inputs': {'images': ['decode', 0], 'filename_prefix': 'enhanced'}},
    }


def run(server, prompt_graph):
    reply = json.loads(request(server + '/prompt', json.dumps({'prompt': prompt_graph}).encode(),
                               {'Content-Type': 'application/json'}))
    pid = reply['prompt_id']
    while True:
        time.sleep(0.5)
        history = get_json(server, '/history/' + pid)
        if pid not in history:
            continue
        entry = history[pid]
        status = entry.get('status', {})
        if status.get('status_str') == 'error':
            raise RuntimeError(json.dumps(status.get('messages', []))[:2000])
        for out in entry.get('outputs', {}).values():
            for img in out.get('images', []):
                q = urllib.parse.urlencode({'filename': img['filename'], 'subfolder': img['subfolder'],
                                            'type': img['type']})
                return request(server + '/view?' + q)
        if status.get('completed'):
            raise RuntimeError('no picture came out')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('input', nargs='?', type=Path, help='folder of PNG pictures')
    ap.add_argument('output', nargs='?', type=Path, help='where the enhanced pictures go (same names)')
    ap.add_argument('--prompt', default=PROMPT, help='the instruction given with every picture')
    ap.add_argument('--prompt-file', type=Path, help='read the instruction from a file')
    ap.add_argument('--steps', type=int, default=4, help='sampling steps (the distilled model is made for 4)')
    # With the first prompt hard pixels worked best (a smoothly enlarged picture read as a blurry photo); with the
    # owner's prompt the model keeps hard pixels as they are (blocky results), and a smoothly enlarged picture comes
    # out with clean edges and surfaces (2026-10-16 comparison on stage textures).
    ap.add_argument('--prescale', default='lanczos',
                    choices=['nearest-exact', 'lanczos', 'bicubic', 'bilinear', 'area'],
                    help='how the picture is enlarged to the model\'s size before it edits it (default: lanczos)')
    ap.add_argument('--megapixels', type=float, default=1.0,
                    help='largest size the model works at (default 1.0; smaller pictures work at 4x, at least 0.25)')
    ap.add_argument('--seed', type=int, default=1, help='the same seed gives the same result for the same picture')
    ap.add_argument('--unet', default='flux-2-klein-4b-fp8.safetensors')
    ap.add_argument('--clip', default='qwen_3_4b.safetensors')
    ap.add_argument('--vae', default='flux2-vae.safetensors')
    ap.add_argument('--server', help='use a running ComfyUI (e.g. http://127.0.0.1:8000) instead of starting one')
    ap.add_argument('--port', type=int, default=8189, help='port of the server it starts (default 8189)')
    ap.add_argument('--info', action='store_true', help='print the installed nodes\' parameters and exit')
    args = ap.parse_args()
    if args.prompt_file:
        args.prompt = args.prompt_file.read_text(encoding='utf-8').strip()

    from PIL import Image  # (ComfyUI's Python has Pillow; so does any Python with the analysis tools)

    with tempfile.TemporaryDirectory(prefix='comfyui-enhance-', ignore_cleanup_errors=True) as temp:
        server = Server(args, temp)
        try:
            if args.info:
                info = get_json(server.url, '/object_info')
                for name in NODES:
                    node = info.get(name)
                    print(name, json.dumps(node['input'] if node else 'NOT INSTALLED')[:600])
                return
            if not args.input or not args.output:
                ap.error('input and output folders are needed')
            args.output.mkdir(parents=True, exist_ok=True)
            pictures = sorted(args.input.glob('*.png'))
            start = time.time()
            failed = 0
            for i, path in enumerate(pictures, 1):
                with Image.open(path) as im:
                    w, h = im.size
                # 4x where it fits in the model's size, at least a quarter of a megapixel.
                megapixels = max(0.25, min(args.megapixels, w * h * 16 / 1e6))
                try:
                    data = run(server.url, graph(upload(server.url, path), args.prompt, args.seed, megapixels, args))
                except Exception as e:
                    print('%s: %s' % (path.name, e), file=sys.stderr)
                    failed += 1
                    continue
                out = args.output / path.name
                out.write_bytes(data)
                # A whole multiple of the input's size, as psptex enhance expects (the model rounds to its own steps).
                with Image.open(out) as res:
                    k = max(1, round(res.width / w))
                    if res.size != (w * k, h * k):
                        res.convert('RGB').resize((w * k, h * k), Image.LANCZOS).save(out)
                eta = (time.time() - start) / i * (len(pictures) - i)
                print('%d/%d %s (%dx%d -> %dx), %.0f s left' % (i, len(pictures), path.name, w, h, k, eta),
                      file=sys.stderr, flush=True)
            if failed:
                sys.exit('%d of %d pictures failed' % (failed, len(pictures)))
        finally:
            server.stop()


if __name__ == '__main__':
    main()
