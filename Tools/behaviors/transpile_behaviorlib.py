#!/usr/bin/env python3
"""One-shot transpiler: the original Alloy server's C# monster behaviours -> Content/Behaviors/<Area>.json.

The original (alloy-server GameServer/Game/Entities/Behaviors/Library/BehaviorLib.<Area>.cs) writes every monster's AI as a C#
expression tree:

    [CharacterBehavior("Hobbit Mage")]
    public static State HobbitMage => new(new State("idle", new EntityWithinTransition("ring1", radius: 12)), new Wander(2.94f), ...);

This script tokenizes those trees, resolves positional / named constructor arguments against the original constructors'
signatures (SIGNATURES below, copied from Behaviors/Actions/*.cs, Transitions/*.cs and Common/Projectiles/ProjectilePaths/*.cs),
and writes the data format of the C++ behaviour engine (Documentation/Migration/AI.md):

    { "format": 1, "source": "BehaviorLib.Lowland.cs",
      "behaviors": { "<object id>": { "loot": [...], "root": { "scripts": [...], "transitions": [...], "states": [...] } } } }

Names follow the original's parameter names, with the cooldown spellings unified (cooldownMS / coolDown / coolDownMS ->
cooldownMs; coolDownOffset / cooldownOffsetMS / cooldownOffset -> cooldownOffsetMs; durationMS -> durationMs; followTimeMS ->
followTimeMs). Only arguments written in the source are emitted; the engine applies the original defaults.

Loot: the original has one active LootDrop (Pirate). Every other monster carries its intended drops as a COMMENTED-OUT old-format
`new CharacterLoot(new ItemLoot(name, chance[, threshold]), new TierLoot(tier, ItemType.X, chance[, threshold]), ...)` table;
those comments are mined as soulbound (per account) loot with the default threshold below.

Run once:  python Tools/behaviors/transpile_behaviorlib.py <alloy-server root> [--out Content/Behaviors]
Prints what could not be converted.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

AREAS = ["Lowland", "Midland", "Highland", "Mountain", "Shore", "GhostShip", "Hermit", "Sphinx", "LotLL"]
DEFAULT_CHARACTER_LOOT_THRESHOLD = 0.01  # the old CharacterLoot tables carry no threshold; "// Threshold 1%" notes in the source

# ----------------------------------------------------------------------------------------------------------------- signatures
# kinds: n number, s string, b bool, p path segment, P ProjectilePath, e enum, t* params (effect, ms) tuples, x script,
#        x* params scripts, s* params strings, l* params loot, m* params path modifiers, a string array
SIGNATURES: dict[str, list[list[tuple[str, str]]]] = {
    # paths
    "LinePath": [[("speed", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    "WavyPath": [[("speed", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    "BoomerangPath": [[("speed", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    "AcceleratePath": [[("speed", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    "DeceleratePath": [[("speed", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    "AmplitudePath": [[("speed", "n"), ("amplitude", "n"), ("frequency", "n"), ("angle", "n"), ("lifetimeMs", "n"),
                       ("timeOffset", "n"), ("mods", "m*")]],
    "CirclePath": [[("rotationsPerSecond", "n"), ("radius", "n"), ("angle", "n"), ("lifetimeMs", "n"), ("timeOffset", "n"),
                    ("mods", "m*")]],
    "ChangeSpeedPath": [[("speed", "n"), ("increment", "n"), ("cooldown", "n"), ("angle", "n"), ("lifetimeMs", "n"),
                         ("cooldownOffset", "n"), ("repeat", "n"), ("timeOffset", "n"), ("mods", "m*")]],
    # actions
    "AOE": [[("radius", "n"), ("damage", "n"), ("cooldownMs", "n"), ("range", "n"), ("cooldownOffset", "n"), ("color", "n"),
             ("targetType", "e"), ("fixedAngle", "n"), ("angleOffset", "n"), ("activateCount", "n"), ("throwTime", "n"),
             ("damageCooldown", "n"), ("damageColor", "n"), ("rotateAngle", "n"), ("effects", "t*")]],
    "BackAndForth": [[("speed", "n"), ("distance", "n")]],
    "Buzz": [[("speed", "n"), ("distance", "n")]],
    "ChangeGround": [[("groundToChange", "a"), ("changeTo", "a"), ("dist", "n")]],
    "ChangeSize": [[("rate", "n"), ("target", "n")]],
    "Charge": [[("speed", "n"), ("range", "n"), ("cooldownMS", "n")]],
    "Circle": [[("rotationsPerSecond", "n"), ("acquireRadius", "n"), ("radius", "n"), ("target", "s")]],
    "ConditionEffectBehavior": [[("effect", "e"), ("durationMS", "n"), ("persist", "b")]],
    "DropPortalOnDeath": [[("portalId", "s"), ("probability", "n"), ("timeout", "n")]],
    "Duration": [[("behavior", "x"), ("duration", "n")]],
    "Flash": [[("color", "n"), ("flashPeriod", "n"), ("flashRepeats", "n")]],
    "Follow": [[("speed", "n"), ("distFromTarget", "n"), ("acquireRange", "n"), ("cooldownMS", "n"), ("cooldownOffsetMS", "n"),
                ("followTimeMS", "n"), ("targetType", "e"), ("target", "s")]],
    "HealGroup": [[("range", "n"), ("group", "s"), ("cooldownMS", "n"), ("healAmount", "n")]],
    "HealSelf": [[("coolDown", "n"), ("amount", "n"), ("percentage", "b"), ("cooldownOffset", "n")]],
    "LootDrop": [[("publicLoot", "b"), ("loots", "l*")]],
    "MoveLine": [[("speed", "n"), ("angle", "n"), ("distance", "n")]],
    "MoveTo": [[("x", "n"), ("y", "n"), ("tilesPerSecond", "n"), ("relative", "b")]],
    "Orbit": [[("speed", "n"), ("radius", "n"), ("acquireRange", "n"), ("target", "s"), ("speedVariance", "n"),
               ("radiusVariance", "n"), ("orbitClockwise", "b"), ("targetPlayer", "b")]],
    "Order": [[("range", "n"), ("children", "s"), ("targetState", "s")]],
    "OrderOnDeath": [[("range", "n"), ("children", "s"), ("targetState", "s")]],
    "Protect": [[("speed", "n"), ("protectee", "s"), ("acquireRange", "n"), ("protectionRange", "n"), ("reprotectRange", "n")]],
    "RemoveObjectOnDeath": [[("objName", "s"), ("range", "n")]],
    "Reproduce": [[("entityName", "s"), ("cooldownMs", "n"), ("maxDensity", "n"), ("densityRadius", "n")]],
    "ReturnToSpawn": [[("speed", "n"), ("distanceFromSpawn", "n")]],
    "Sequence": [[("behaviors", "x*")]],
    "SetAltTexture": [[("minValue", "n"), ("maxValue", "n"), ("cooldown", "n"), ("loop", "b")]],
    "Shoot": [
        # inline projectile (ProjectilePathSegment), targeted
        [("maxRadius", "n"), ("path", "p"), ("count", "n"), ("shootAngle", "n"), ("projType", "n"), ("fixedAngle", "n"),
         ("rotateAngle", "n"), ("angleOffset", "n"), ("predictive", "n"), ("coolDownOffset", "n"), ("cooldownMS", "n"),
         ("targeted", "b"), ("projName", "s"), ("lifetimeMs", "n"), ("minDamage", "n"), ("maxDamage", "n"), ("damage", "n"),
         ("xOffset", "n"), ("yOffset", "n"), ("size", "n"), ("multiHit", "b"), ("passesCover", "b"), ("armorPiercing", "b"),
         ("minRadius", "n"), ("effects", "t*")],
        # inline projectile, targetType
        [("maxRadius", "n"), ("path", "p"), ("count", "n"), ("shootAngle", "n"), ("projType", "n"), ("fixedAngle", "n"),
         ("rotateAngle", "n"), ("angleOffset", "n"), ("predictive", "n"), ("coolDownOffset", "n"), ("cooldownMS", "n"),
         ("targetType", "e"), ("projName", "s"), ("lifetimeMs", "n"), ("minDamage", "n"), ("maxDamage", "n"), ("damage", "n"),
         ("xOffset", "n"), ("yOffset", "n"), ("size", "n"), ("multiHit", "b"), ("passesCover", "b"), ("armorPiercing", "b"),
         ("minRadius", "n"), ("effects", "t*")],
        # inline projectile with a multi-segment ProjectilePath
        [("maxRadius", "n"), ("path", "P"), ("count", "n"), ("shootAngle", "n"), ("projType", "n"), ("fixedAngle", "n"),
         ("rotateAngle", "n"), ("angleOffset", "n"), ("predictive", "n"), ("coolDownOffset", "n"), ("cooldownMS", "n"),
         ("targeted", "b"), ("projName", "s"), ("minDamage", "n"), ("maxDamage", "n"), ("damage", "n"), ("xOffset", "n"),
         ("yOffset", "n"), ("size", "n"), ("multiHit", "b"), ("passesCover", "b"), ("armorPiercing", "b"), ("minRadius", "n"),
         ("effects", "t*")],
        # the object's own XML <Projectile id=N>
        [("maxRadius", "n"), ("count", "n"), ("shootAngle", "n"), ("projectilePropsId", "n"), ("fixedAngle", "n"),
         ("rotateAngle", "n"), ("angleOffset", "n"), ("predictive", "n"), ("coolDownOffset", "n"), ("cooldownMS", "n"),
         ("targeted", "b"), ("xOffset", "n"), ("yOffset", "n"), ("minRadius", "n")],
    ],
    "Spawn": [
        [("entityName", "s"), ("x", "n"), ("y", "n"), ("cooldownMs", "n"), ("cooldownOffsetMs", "n"), ("maxSpawnsPerReset", "n"),
         ("minSpawnCount", "n"), ("maxSpawnCount", "n"), ("group", "s"), ("maxDensity", "n"), ("densityRadius", "n")],
        [("entityName", "s"), ("minX", "n!"), ("maxX", "n!"), ("minY", "n!"), ("maxY", "n!"), ("cooldownMs", "n"),
         ("cooldownOffsetMs", "n"), ("maxSpawnsPerReset", "n"), ("minSpawnCount", "n"), ("maxSpawnCount", "n"), ("group", "s"),
         ("maxDensity", "n"), ("densityRadius", "n")],
    ],
    "SpawnSetpieceOnDeath": [[("setpiece", "s"), ("useSpawnPoint", "b")]],
    "StayAwayFrom": [[("speed", "n"), ("distFromTarget", "n"), ("acquireRange", "n"), ("cooldownMS", "n"), ("cooldownOffsetMS", "n"),
                      ("followTimeMS", "n"), ("targetType", "e"), ("target", "s")]],
    "Suicide": [[("delay", "n")]],
    "Swirl": [[("speed", "n"), ("radius", "n"), ("acquireRange", "n"), ("targeted", "b")]],
    "Taunt": [[("text", "s"), ("coolDownMS", "n"), ("probability", "n")]],
    "Timed": [[("period", "n"), ("behaviors", "x*")]],
    "TossObject": [[("child", "s"), ("range", "n"), ("angle", "n"), ("cooldownMS", "n"), ("cooldownOffsetMS", "n"), ("tossInvis", "b"),
                    ("probability", "n"), ("group", "s"), ("minAngle", "n"), ("maxAngle", "n"), ("minRange", "n"), ("maxRange", "n"),
                    ("densityRange", "n"), ("maxDensity", "n"), ("region", "e"), ("regionRange", "n"), ("targeted", "b")]],
    "Transform": [[("target", "s")]],
    "TransformOnDeath": [[("target", "s"), ("min", "n"), ("max", "n"), ("probability", "n")]],
    "Wander": [[("speed", "n"), ("distance", "n"), ("distanceFromSpawn", "n"), ("cooldownMs", "n"), ("ease", "e")]],
    # transitions
    "DamageTakenTransition": [[("damage", "n"), ("targetState", "s")]],
    "EntitiesNotWithinTransition": [[("radius", "n"), ("targetStates", "s"), ("targets", "s*")]],
    "EntitiesWithinTransition": [[("radius", "n"), ("targetStates", "s"), ("targets", "s*")]],
    "EntityHpLessTransition": [[("dist", "n"), ("entity", "s"), ("threshold", "n"), ("targetState", "s")]],
    "EntityNotWithinTransition": [
        [("targetState", "s"), ("target", "s"), ("radius", "n"), ("transitionType", "e")],
        [("target", "s"), ("radius", "n"), ("transitionType", "e"), ("targetStates", "s*")],
    ],
    "EntityWithinTransition": [
        [("targetState", "s"), ("target", "s"), ("radius", "n"), ("transitionType", "e")],
        [("target", "s"), ("radius", "n"), ("transitionType", "e"), ("targetStates", "s*")],
    ],
    "HpLessTransition": [[("threshold", "n"), ("targetState", "s"), ("transitionType", "e")]],
    "NotMovingTransition": [[("targetState", "s"), ("delay", "n")]],
    "OnParentDeathTransition": [[("targetState", "s")]],
    "PlayerTextTransition": [[("targetState", "s"), ("regex", "s"), ("ignoreCase", "b")]],
    "TimedTransition": [
        [("time", "n"), ("targetState", "s")],
        [("time", "n"), ("transitionType", "e"), ("targetStates", "s*")],
    ],
    # loot
    "ItemLoot": [[("objectId", "s"), ("threshold", "n"), ("chance", "n")]],
    "TierLoot": [[("tier", "n"), ("itemType", "e"), ("threshold", "n"), ("chance", "n")]],
}

PATH_TYPES = {"LinePath": "Line", "WavyPath": "Wavy", "BoomerangPath": "Boomerang", "AmplitudePath": "Amplitude",
              "AcceleratePath": "Accelerate", "DeceleratePath": "Decelerate", "CirclePath": "Circle",
              "ChangeSpeedPath": "ChangeSpeed"}
DEATH_AND_ONCE = {"DropPortalOnDeath", "TransformOnDeath", "OrderOnDeath", "SpawnSetpieceOnDeath", "RemoveObjectOnDeath"}
RENAMES = {"cooldownMS": "cooldownMs", "coolDown": "cooldownMs", "coolDownMS": "cooldownMs", "coolDownOffset": "cooldownOffsetMs",
           "cooldownOffsetMS": "cooldownOffsetMs", "cooldownOffset": "cooldownOffsetMs", "durationMS": "durationMs",
           "followTimeMS": "followTimeMs"}


class ParseError(Exception):
    pass


# ----------------------------------------------------------------------------------------------------------------- tokenizer
TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<str>@?"(?:[^"\\]|\\.)*")
  | (?P<num>0[xX][0-9a-fA-F]+|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFdDmM]?)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>=>|[()\[\]{},:.;=+\-*/<>?!])
""", re.VERBOSE)


def strip_comments(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def tokenize(text: str) -> list[tuple[str, str]]:
    tokens = []
    pos = 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise ParseError(f"unexpected character {text[pos]!r} near {text[max(0, pos - 30):pos + 30]!r}")
        pos = m.end()
        kind = m.lastgroup
        if kind == "ws":
            continue
        tokens.append((kind, m.group()))
    return tokens


# ----------------------------------------------------------------------------------------------------------------- parser
class New:
    def __init__(self, name: str, args: list):
        self.name = name
        self.args = args  # list of (name or None, value)


class Enum:
    def __init__(self, path: str):
        self.path = path

    @property
    def member(self) -> str:
        return self.path.split(".")[-1]


class Parser:
    def __init__(self, tokens):
        self.t = tokens
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else ("eof", "")

    def take(self, value=None):
        tok = self.peek()
        if value is not None and tok[1] != value:
            raise ParseError(f"expected {value!r}, got {tok[1]!r}")
        self.i += 1
        return tok

    def expr(self):
        left = self.unary()
        while self.peek()[1] in ("+", "-", "*", "/"):
            op = self.take()[1]
            right = self.unary()
            if not isinstance(left, (int, float)) or not isinstance(right, (int, float)):
                if op == "+" and isinstance(left, str) and isinstance(right, str):
                    left = left + right
                    continue
                raise ParseError(f"cannot evaluate {left!r} {op} {right!r}")
            left = {"+": left + right, "-": left - right, "*": left * right, "/": left / right}[op]
        return left

    def unary(self):
        if self.peek()[1] == "-":
            self.take()
            v = self.unary()
            if not isinstance(v, (int, float)):
                raise ParseError("minus on a non-number")
            return -v
        if self.peek()[1] == "(" and self.peek(1)[0] == "id" and self.peek(2)[1] == ")" and self.peek(3)[0] in ("num", "id", "op"):
            # a cast like (byte)3 or (float)x
            if self.peek(1)[1] in ("byte", "int", "float", "double", "ushort", "short"):
                self.i += 3
                return self.unary()
        return self.primary()

    def primary(self):
        kind, value = self.peek()
        if kind == "num":
            self.take()
            v = value.rstrip("fFdDmM") if not value.lower().startswith("0x") else value
            if v.lower().startswith("0x"):
                return int(v, 16)
            return float(v) if ("." in v or "e" in v.lower()) else int(v)
        if kind == "str":
            self.take()
            raw = value[2:-1] if value.startswith("@") else value[1:-1]
            return raw if value.startswith("@") else bytes(raw, "utf-8").decode("unicode_escape")
        if kind == "id" and value == "new":
            self.take()
            if self.peek()[1] == "[":
                self.take("[")
                self.take("]")
                self.take("{")
                items = []
                while self.peek()[1] != "}":
                    items.append(self.expr())
                    if self.peek()[1] == ",":
                        self.take()
                self.take("}")
                return items
            name = "State"
            if self.peek()[0] == "id":
                name = self.take()[1]
                while self.peek()[1] == ".":
                    self.take()
                    name = self.take()[1]
                if self.peek()[1] == "<":  # generic: new List<ProjectilePathSegment> { ... }
                    while self.take()[1] != ">":
                        pass
            if self.peek()[1] == "{":  # collection initializer
                self.take("{")
                items = []
                while self.peek()[1] != "}":
                    items.append(self.expr())
                    if self.peek()[1] == ",":
                        self.take()
                self.take("}")
                return New(name, [(None, items)])
            return New(name, self.arguments())
        if kind == "id":
            self.take()
            if value in ("true", "false"):
                return value == "true"
            if value == "null":
                return None
            path = value
            while self.peek()[1] == ".":
                self.take()
                path += "." + self.take()[1]
            if path == "int.MaxValue":
                return 2147483647
            return Enum(path)
        if value == "[":  # C# 12 collection expression
            self.take()
            items = []
            while self.peek()[1] != "]":
                items.append(self.expr())
                if self.peek()[1] == ",":
                    self.take()
            self.take("]")
            return items
        if value == "(":
            self.take()
            items = [self.expr()]
            while self.peek()[1] == ",":
                self.take()
                items.append(self.expr())
            self.take(")")
            return tuple(items) if len(items) > 1 else items[0]
        raise ParseError(f"unexpected token {value!r}")

    def arguments(self):
        self.take("(")
        args = []
        while self.peek()[1] != ")":
            name = None
            if self.peek()[0] == "id" and self.peek(1)[1] == ":":
                name = self.take()[1]
                self.take(":")
            args.append((name, self.expr()))
            if self.peek()[1] == ",":
                self.take()
            elif self.peek()[1] != ")":
                raise ParseError(f"expected ',' or ')', got {self.peek()[1]!r}")
        self.take(")")
        return args


# ----------------------------------------------------------------------------------------------------------------- converter
def kind_of(value) -> str:
    if isinstance(value, bool):
        return "b"
    if isinstance(value, (int, float)):
        return "n"
    if isinstance(value, str) or value is None:
        return "s"
    if isinstance(value, Enum):
        return "e"
    if isinstance(value, tuple):
        return "t"
    if isinstance(value, list):
        return "a"
    if isinstance(value, New):
        if value.name in PATH_TYPES:
            return "p"
        if value.name in ("ProjectilePath", "CombinedPath"):
            return "P"
        if value.name in ("ItemLoot", "TierLoot"):
            return "l"
        return "x"
    return "?"


def accepts(param_kind: str, value) -> bool:
    k = kind_of(value)
    base = param_kind.rstrip("*!")
    if base == "n":
        return k == "n"
    if base == "s":
        return k == "s"
    if base == "e":
        return k == "e"
    if base == "x":
        return k in ("x",)
    return k == base


def bind(name: str, args: list) -> dict:
    """Resolves a constructor call against the original overloads, like C# overload resolution (first applicable wins)."""
    sigs = SIGNATURES.get(name)
    if sigs is None:
        raise ParseError(f"unknown construct {name}")
    errors = []
    for sig in sigs:
        bound = {}
        ok = True
        positional = [v for n, v in args if n is None]
        named = [(n, v) for n, v in args if n is not None]
        pi = 0
        for pname, pkind in sig:
            if pkind.endswith("*"):
                rest = positional[pi:]
                if not all(accepts(pkind, v) for v in rest):
                    ok = False
                bound[pname] = rest
                pi = len(positional)
                break
            if pi < len(positional):
                if not accepts(pkind, positional[pi]):
                    ok = False
                    break
                bound[pname] = positional[pi]
                pi += 1
        if not ok or pi < len(positional):
            errors.append(f"positional args do not fit {[p for p, _ in sig]}")
            continue
        sig_names = {p: k for p, k in sig}
        for n, v in named:
            if n not in sig_names:
                ok = False
                break
            if sig_names[n].endswith("*"):
                bound[n] = list(v) if isinstance(v, list) else [v]
            elif not accepts(sig_names[n], v):
                ok = False
                break
            else:
                bound[n] = v
        if not ok:
            errors.append("named args do not fit")
            continue
        if any(k.endswith("!") and p not in bound for p, k in sig):
            continue
        if "path" in sig_names and "path" not in bound and sig_names["path"] in ("p", "P"):
            continue  # an inline-projectile Shoot needs its path
        return bound
    raise ParseError(f"{name}: no overload fits ({'; '.join(errors)})")


def jsonable(v):
    if isinstance(v, Enum):
        return v.member
    if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
        return int(v)
    return v


class Converter:
    def __init__(self, behavior_name: str, problems: list[str]):
        self.behavior = behavior_name
        self.problems = problems
        self.loot: list = []

    def problem(self, text: str):
        self.problems.append(f"{self.behavior}: {text}")

    def path(self, node: New) -> list:
        if node.name in PATH_TYPES:
            b = bind(node.name, node.args)
            seg = {"type": PATH_TYPES[node.name]}
            for k, v in b.items():
                if k == "mods":
                    if v:
                        seg["boomerang"] = True
                    continue
                seg[k] = jsonable(v)
            return [seg]
        if node.name == "ProjectilePath":
            items = node.args[0][1] if node.args else []
            if not isinstance(items, list):
                raise ParseError("ProjectilePath expects a list of segments")
            out = []
            for it in items:
                out += self.path(it)
            return out
        if node.name == "CombinedPath":
            time_offset = None
            segs = []
            for n, v in node.args:
                if n == "timeOffset" or (n is None and isinstance(v, (int, float)) and not segs):
                    time_offset = v
                else:
                    segs += self.path(v)
            seg = {"type": "Combined", "segments": segs}
            if time_offset:
                seg["timeOffset"] = jsonable(time_offset)
            return [seg]
        raise ParseError(f"not a path: {node.name}")

    def renamed(self, b: dict, skip=()) -> dict:
        out = {}
        for k, v in b.items():
            if k in skip:
                continue
            if isinstance(v, New) or (isinstance(v, list) and v and isinstance(v[0], New)):
                continue
            if isinstance(v, tuple):
                continue
            out[RENAMES.get(k, k)] = [jsonable(x) for x in v] if isinstance(v, list) else jsonable(v)
        return out

    def effects(self, items) -> list:
        out = []
        for t in items or []:
            if not isinstance(t, tuple) or len(t) != 2:
                raise ParseError("effects must be (ConditionEffectIndex, ms) tuples")
            out.append({"effect": jsonable(t[0]), "durationMs": jsonable(t[1])})
        return out

    def loot_entry(self, node: New, old_format: bool) -> dict:
        if old_format:
            # Old format: ItemLoot(name, chance[, threshold]) / TierLoot(tier, type, chance[, threshold]).
            vals = [v for _, v in node.args]
            if node.name == "ItemLoot":
                return {"type": "Item", "id": vals[0], "chance": jsonable(vals[1]),
                        "threshold": jsonable(vals[2]) if len(vals) > 2 else DEFAULT_CHARACTER_LOOT_THRESHOLD}
            if node.name == "TierLoot":
                return {"type": "Tier", "tier": jsonable(vals[0]), "itemType": jsonable(vals[1]), "chance": jsonable(vals[2]),
                        "threshold": jsonable(vals[3]) if len(vals) > 3 else DEFAULT_CHARACTER_LOOT_THRESHOLD}
        b = bind(node.name, node.args)
        if node.name == "ItemLoot":
            return {"type": "Item", "id": b["objectId"], "threshold": jsonable(b["threshold"]), "chance": jsonable(b["chance"])}
        return {"type": "Tier", "tier": jsonable(b["tier"]), "itemType": jsonable(b["itemType"]), "threshold": jsonable(b["threshold"]),
                "chance": jsonable(b["chance"])}

    def script(self, node: New):
        name = node.name
        b = bind(name, node.args)
        if name == "LootDrop":
            items = []
            for l in b.get("loots", []):
                items.append(self.loot_entry(l, old_format=False))
            self.loot.append({"public": bool(b["publicLoot"]), "items": items})
            return None
        if name == "Shoot":
            out = {"type": "Shoot"}
            if "path" in b:
                proj = {"id": b.get("projName") or None, "path": self.path(b["path"])}
                if not proj["id"]:
                    if "projType" in b:
                        proj["type"] = jsonable(b["projType"])
                    else:
                        proj["id"] = "Blade"  # the original falls back to Blade
                for k in ("damage", "minDamage", "maxDamage", "lifetimeMs", "size", "multiHit", "passesCover", "armorPiercing"):
                    if k in b:
                        proj[k] = jsonable(b[k])
                if b.get("effects"):
                    proj["effects"] = self.effects(b["effects"])
                if proj["id"] is None:
                    del proj["id"]
                out["projectile"] = proj
                rest = self.renamed(b, skip=("path", "projName", "projType", "damage", "minDamage", "maxDamage", "lifetimeMs", "size",
                                             "multiHit", "passesCover", "armorPiercing", "effects"))
            else:
                rest = self.renamed(b, skip=("projectilePropsId",))
                out["projectileIndex"] = jsonable(b.get("projectilePropsId", 0))
            out.update(rest)
            return out
        if name in ("Timed", "Sequence"):
            out = {"type": name}
            if "period" in b:
                out["period"] = jsonable(b["period"])
            out["scripts"] = [s for s in (self.script(x) for x in b.get("behaviors", [])) if s is not None]
            return out
        if name == "Duration":
            inner = self.script(b["behavior"])
            return {"type": "Duration", "duration": jsonable(b["duration"]), "script": inner}
        out = {"type": name}
        out.update(self.renamed(b))
        if name == "AOE" and b.get("effects"):
            out["effects"] = self.effects(b["effects"])
        return out

    def transition(self, node: New):
        b = bind(node.name, node.args)
        out = {"type": node.name[: -len("Transition")]}
        targets = b.get("targetStates", b.get("targetState"))
        if isinstance(targets, list):
            out["to"] = targets if len(targets) != 1 else targets[0]
        else:
            out["to"] = targets
        if "transitionType" in b:
            out["mode"] = jsonable(b["transitionType"])
        for k, v in b.items():
            if k in ("targetStates", "targetState", "transitionType"):
                continue
            out[k] = [jsonable(x) for x in v] if isinstance(v, list) else jsonable(v)
        return out

    def state(self, node: New, top: bool) -> dict:
        st: dict = {}
        args = node.args
        if args and args[0][0] is None and isinstance(args[0][1], str):
            st["name"] = args[0][1]
            args = args[1:]
        elif not top:
            self.problem("a child state without a name")
        scripts, transitions, states = [], [], []
        for n, v in args:
            if not isinstance(v, New):
                self.problem(f"unexpected state child {v!r}")
                continue
            try:
                if v.name == "State":
                    states.append(self.state(v, False))
                elif v.name.endswith("Transition"):
                    transitions.append(self.transition(v))
                else:
                    s = self.script(v)
                    if s is not None:
                        scripts.append(s)
            except ParseError as e:
                self.problem(f"skipped {v.name}: {e}")
        if scripts:
            st["scripts"] = scripts
        if transitions:
            st["transitions"] = transitions
        if states:
            st["states"] = states
        return st


ATTRIBUTE = re.compile(r'\[CharacterBehavior\("([^"]+)"\)\]')
MEMBER = re.compile(r"public\s+static\s+State\s+\w+\s*=>")


def mine_commented_loot(raw_body: str, conv: Converter) -> list:
    """Old-format `// new CharacterLoot(...)` tables inside a behaviour (commented out in the original)."""
    loot = []
    lines = raw_body.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if re.match(r"^//\s*new CharacterLoot\(", line):
            depth = 0
            buf = []
            while i < len(lines):
                l = lines[i].strip()
                if not l.startswith("//"):
                    break
                l = re.sub(r"^(//\s*)+", "", l)
                l = re.sub(r"\)\s*//.*$", ")", l)  # trailing notes like "// Threshold 1%"
                if l.startswith("new CharacterLoot(") and "//" in l:
                    l = l[: l.index("//")]
                buf.append(l)
                depth += l.count("(") - l.count(")")
                i += 1
                if depth <= 0:
                    break
            text = " ".join(buf)
            # The tables were commented out line by line, so separators may be missing: take every entry on its own.
            items = []
            for m in re.finditer(r"new (ItemLoot|TierLoot)\(", text):
                depth = 0
                k = m.end() - 1
                while k < len(text):
                    depth += {"(": 1, ")": -1}.get(text[k], 0)
                    if depth == 0:
                        break
                    k += 1
                try:
                    node = Parser(tokenize(text[m.start():k + 1])).expr()
                    items.append(conv.loot_entry(node, old_format=True))
                except (ParseError, IndexError) as e:
                    conv.problem(f"commented loot entry not parsed ({e})")
            for m in re.finditer(r"LootTemplates\.(\w+)", text):
                conv.problem(f"commented loot template LootTemplates.{m.group(1)} has no definition in the original, skipped")
            if items:
                loot.append({"public": False, "items": items})
            continue
        i += 1
    return loot


def convert_file(path: Path, problems: list[str]) -> dict:
    raw = path.read_text(encoding="utf-8-sig")
    behaviors = {}
    # Split by members: every `public static State X =>` with its attributes right before it.
    pos = 0
    pending_names: list[str] = []
    while True:
        m_attr = ATTRIBUTE.search(raw, pos)
        m_member = MEMBER.search(raw, pos)
        if not m_member:
            break
        if m_attr and m_attr.start() < m_member.start():
            # Skip attributes that are commented out.
            line_start = raw.rfind("\n", 0, m_attr.start()) + 1
            if not raw[line_start:m_attr.start()].strip().startswith("//"):
                pending_names.append(m_attr.group(1))
            pos = m_attr.end()
            continue
        # Body: from after "=>" to the matching ";" at depth 0 (comments stripped for the structure, kept for loot).
        start = m_member.end()
        depth = 0
        j = start
        in_str = False
        while j < len(raw):
            c = raw[j]
            if in_str:
                if c == "\\":
                    j += 2
                    continue
                if c == '"':
                    in_str = False
            elif raw.startswith("//", j):
                j = raw.find("\n", j)
                continue
            elif raw.startswith("/*", j):
                j = raw.find("*/", j) + 2
                continue
            elif c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            j += 1
        body = raw[start:j]
        pos = j + 1
        names = pending_names
        pending_names = []
        if not names:
            continue
        conv = Converter(names[0], problems)
        try:
            node = Parser(tokenize(strip_comments(body))).expr()
            if not isinstance(node, New) or node.name != "State":
                raise ParseError("the behaviour is not a State")
            root = conv.state(node, True)
        except ParseError as e:
            problems.append(f"{names[0]}: not converted ({e})")
            continue
        loot = conv.loot + mine_commented_loot(body, conv)
        entry = {}
        if loot:
            entry["loot"] = loot
        entry["root"] = root
        for name in names:
            if name in behaviors:
                problems.append(f"{name}: defined twice, the first definition is kept")
                continue
            behaviors[name] = entry
    return behaviors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("alloy_server", type=Path, help="root of the original alloy-server clone")
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "Content" / "Behaviors")
    ap.add_argument("--areas", nargs="*", default=AREAS)
    args = ap.parse_args()
    lib = args.alloy_server / "GameServer" / "Game" / "Entities" / "Behaviors" / "Library"
    args.out.mkdir(parents=True, exist_ok=True)
    all_problems: list[str] = []
    total = 0
    for area in args.areas:
        src = lib / f"BehaviorLib.{area}.cs"
        problems: list[str] = []
        behaviors = convert_file(src, problems)
        total += len(behaviors)
        doc = {"format": 1, "source": src.name, "behaviors": behaviors}
        (args.out / f"{area}.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{area}: {len(behaviors)} behaviours, {len(problems)} problems")
        all_problems += [f"  {area}: {p}" for p in problems]
    if all_problems:
        print("Not converted:")
        print("\n".join(all_problems))
    print(f"{total} behaviours written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
