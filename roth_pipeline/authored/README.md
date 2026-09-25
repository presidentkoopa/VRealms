# authored/

Hand-made data. **A pipeline rebuild must never write here.**

Everything in `../generated/` can be deleted and rebuilt from the original game
files. Nothing here can. That is the whole point of the split: regenerating a
map has to stay safe to do forever, or the project drifts back into hand-editing
levels — which is exactly what the pipeline exists to avoid.

## How to key your data

Every prop the converter places carries its origin in the map:

    thing { ... user_roth_sector = 47; user_roth_object = 2; }

Those two numbers identify the object in the original game data and do not
change when the converter does. Author against them.

Suggested shape, one file per map (`authored/STUDY1.json`):

    {
      "objects": {
        "47:2": { "mass": 12, "grabbable": true, "grab": [0, 0, 18] },
        "47:3": { "mass": 60, "grabbable": false }
      }
    }

Nothing consumes this yet — it is written down now so the IDs exist before
there is anything to attach to them. Retrofitting stable identity later is
painful; carrying it from the start is free.
