# Training data for kind.bin

`still/bin/kind.bin` is the 2D-vs-3D classifier behind the Images & video tab's
"looks like" label. It is a logistic head on frozen MobileNetV3-Large
features, fitted by `kind-train.py` on frames from short video clips.

## What was used

- 123 clips, listed by file name in `kind-clips.txt`: 53 labelled `2d_`
  (cel and hand-drawn animation, anime, 2D and painted games) and 70 labelled
  `3d_` (3D animation, Blender films, 3D games, film, TV, camera footage).
- Up to 10 frames per clip, scaled to 480×270. Only the fitted weights
  (6 MB) are in this repository.
- **No clip and no frame is distributed.** The file names in
  `kind-clips.txt` name what each clip shows; they are not the clips.

## Provenance

| Field | Status |
|---|---|
| Where the clips came from | **Not recorded.** To be filled in by the maintainer. |
| Licence of each clip | **Not recorded.** |

Many clip names refer to commercial films, series and games. Using short
excerpts to fit a small classifier, and shipping only the weights, is a
grey area, not a settled right. Until the sources are recorded, treat
`kind.bin` as trained on material whose licence is unknown.

## Replacing it

The weights can be rebuilt from footage that is known to be usable, such as
your own recordings or openly licensed videos (Creative Commons, Blender
Foundation open movies, public-domain animation), without changing any
code:

```sh
# name each clip 2d_<anything>.mp4 or 3d_<anything>.mp4
python3 kind-train.py prep  data.npz /path/to/clips
python3 kind-train.py export data.npz ../bin/kind.bin
```

When that is done, replace `kind-clips.txt` with the new list. Fill in
the table above, and record each clip's source and licence in a third column
of `kind-clips.txt` (`name<TAB>source URL<TAB>licence`).
