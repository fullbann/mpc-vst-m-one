"""Run by gen_vst.py after the skin is built (vst.json "skin_post"): type roles for the knobs --
the value (what you read while turning) in Fullban aqua, bold; the name quiet grey. Fails loudly if the knob
definitions it expects are gone, so a builder change can't silently drop the styling."""
import json, os, sys
tui = os.path.join(sys.argv[1], "Plugin Skins", "TUI.json")
d = json.load(open(tui))
n = 0
for c in d["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
    if not str(c.get("key", "")).startswith("shKnob"):
        continue
    for cd in c["value"].get("componentsData", []):
        data = cd.get("componentData", {}).get("data", {})
        ts = data.get("textStyle")
        if data.get("type") == "Value" and ts:
            ts["colour"] = "ff2fd2c6"; ts["font"]["style"] = "Bold"; n += 1
        elif data.get("type") == "Name" and ts:
            ts["colour"] = "ff8a95a5"; ts["font"]["height"] = 20.0; ts["font"]["style"] = "SemiBold"
if not n:
    raise SystemExit("skin_post: no knob Value labels found -- update skin_post.py")
# The selected control (the one the data wheel and the +/- keys drive) gets a soft aqua highlight: MPC draws a control's
# Focus subcomponent only while it has focus ("WhenFocussed"); the builder leaves it transparent.
nf = 0
def focus(o):
    global nf
    if isinstance(o, dict):
        cd = o.get("componentData")
        if isinstance(cd, dict) and cd.get("type") == "Focus":
            cd["data"].update({"backgroundColour": "1a2fd2c6", "outlineColour": "cc2fd2c6",
                               "outlineThickness": 2.0, "backgroundInset": 3.0})
            nf += 1
        for v in o.values(): focus(v)
    elif isinstance(o, list):
        for v in o: focus(v)
focus(d)
if not nf:
    raise SystemExit("skin_post: no Focus subcomponents found -- update skin_post.py")
json.dump(d, open(tui, "w"), indent=1)
print("skin_post: focus highlight on %d control definitions" % nf)
print("skin_post: styled %d knob value labels" % n)
