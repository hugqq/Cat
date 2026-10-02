from __future__ import annotations

import shutil
from pathlib import Path

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = Path(r"C:\Users\33047\Desktop\cat\cat_sprite_16frames")
OUTPUT_ROOT = PROJECT_ROOT / "public" / "pets" / "v2"

STATES = (
    "resting_candidate_sleep",
    "walking_candidate",
    "impact_candidate",
    "vigorous_activity",
    "other_activity",
)


def main() -> None:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    for state_name in STATES:
        source_path = SOURCE_ROOT / "previews" / f"{state_name}.gif"
        with Image.open(source_path) as image:
            if image.n_frames != 16:
                raise RuntimeError(f"{state_name}: expected 16 frames, got {image.n_frames}")

        output_path = OUTPUT_ROOT / f"{state_name}.gif"
        shutil.copy2(source_path, output_path)
        print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
