import sys
import json
import numpy as np

from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent
DATASET_PATH = BASE_DIR / 'references.json'
REFS_PATH = BASE_DIR / 'refs.bin'
LABELS_PATH = BASE_DIR / 'labels.bin'

VDIM = 14
VDIM_PADDED = 16
N_REFS = 100000
SCALE = 8192


def load_dataset(path):
    with open(path) as f:
        data = json.load(f)
    X = np.array([e['vector'] for e in data], dtype=np.float32)
    y = np.array([1 if e['label'] == 'fraud' else 0 for e in data], dtype=np.uint8)
    return X, y


def quantize(X):
    out = np.zeros((len(X), VDIM_PADDED), dtype=np.int16)
    for j in range(VDIM):
        col = X[:, j].astype(np.float32)
        clipped = np.clip(col, 0.0, 1.0)
        q = (clipped * SCALE + 0.5).astype(np.int32)
        q = np.minimum(q, SCALE)
        if j in (5, 6):
            q = np.where(col < 0, -SCALE, q)
        out[:, j] = q.astype(np.int16)
    return out


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DATASET_PATH

    print(f'Carregando {src}')
    X, y = load_dataset(src)
    if len(X) != N_REFS:
        raise Exception(f'esperado {N_REFS} refs, achei {len(X)}')

    print(f'  {len(X)} vetores | {y.sum()} fraudes | {(1 - y).sum()} legítimas')

    REFS_PATH.write_bytes(quantize(X).tobytes())
    LABELS_PATH.write_bytes(y.tobytes())

    print(f'  refs.bin   {REFS_PATH.stat().st_size / 1024 / 1024:6.2f} MB')
    print(f'  labels.bin {LABELS_PATH.stat().st_size / 1024:6.0f} KB')


if __name__ == '__main__':
    main()
