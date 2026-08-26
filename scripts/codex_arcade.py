#!/usr/bin/env python3
"""Terminal twin-stick shooter controlled by the Codex Micro / Creator Micro 2.

Reads canonical `gamepad-state` / `gamepad-pulse` JSONL from the
`codex_micro_probe gamepad` subprocess (spawned automatically) and falls back
to keyboard input whenever the device feed is absent, so the game is always
playable. This file is self-contained (stdlib only) and has no import
dependency on the codex_micro_probe package, so it can be copied and run
anywhere Python 3.9+ is available.

Controls (device):
    AG00-AG05        select weapon 0-5
    dial turn        cycle weapon
    encoder press     fire selected weapon
    ACT06 (lab)       bomb: clears nearby asteroids (cooldown)
    ACT07 (yolo)      boost thrust while held
    ACT08 (yeet)      heavy shot (independent cooldown)
    ACT09 (magic)     shield while held
    mic (ACT10/11)    pause
    menu-grid (ACT12) restart on game over, else toggles debug overlay
    joystick          thrust direction and aim

Controls (keyboard fallback):
    arrows / WASD     thrust direction and aim
    space             fire
    1-6               select weapon
    [ / ]             cycle weapon
    b                 bomb
    f                 boost (hold)
    g                 shield (hold)
    p                 pause
    r                 restart
    q / ESC           quit

Run:
    python3 codex_arcade.py
    python3 codex_arcade.py --keyboard-only
    python3 codex_arcade.py --probe-dir /path/to/codex-micro-analysis
"""

from __future__ import annotations

import argparse
import curses
import json
import math
import os
import random
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Controller ingestion
# ---------------------------------------------------------------------------

# Canonical button indices emitted by codex_micro_probe.gamepad.GamepadMapper.
WEAPON_BUTTONS = {0, 1, 2, 3, 4, 5}
BOMB_BUTTON = 6
BOOST_BUTTON = 7
HEAVY_BUTTON = 8
SHIELD_BUTTON = 9
PAUSE_BUTTON = 10
MENU_BUTTON = 11
FIRE_BUTTON = 12

DEFAULT_PROBE_DIR = Path.home() / "Dsco" / "codex-micro-analysis"


@dataclass
class InputState:
    x: float = 0.0
    y: float = 0.0
    buttons: set[int] = field(default_factory=set)
    dial: int = 0
    connected: bool = False
    lock: threading.Lock = field(default_factory=threading.Lock)

    def snapshot(self) -> tuple[float, float, set[int], int, bool]:
        with self.lock:
            dial = self.dial
            self.dial = 0
            return self.x, self.y, set(self.buttons), dial, self.connected


def controller_thread(state: InputState, cmd: list[str], cwd: Path | None, env: dict) -> None:
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd) if cwd else None,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
    except OSError:
        return
    with state.lock:
        state.connected = True
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            event_type = event.get("event_type")
            if event_type == "gamepad-state":
                axes = event.get("axes", {})
                with state.lock:
                    state.x = float(axes.get("x", 0.0))
                    state.y = float(axes.get("y", 0.0))
                    state.buttons = set(event.get("buttons", []))
            elif event_type == "gamepad-pulse":
                with state.lock:
                    state.dial += 1 if event.get("control") == "dial-right" else -1
    except Exception:
        pass
    finally:
        with state.lock:
            state.connected = False
            state.x = state.y = 0.0
            state.buttons = set()
        try:
            proc.terminate()
        except Exception:
            pass


def start_controller(probe_dir: Path | None, controller_cmd: str | None) -> InputState:
    state = InputState()
    if controller_cmd:
        cmd = controller_cmd.split()
        cwd = probe_dir
        env = os.environ.copy()
    elif probe_dir and probe_dir.is_dir():
        cmd = [sys.executable, "-m", "codex_micro_probe", "gamepad", "--duration", "0"]
        cwd = probe_dir
        env = os.environ.copy()
        env["PYTHONPATH"] = str(probe_dir / "src")
    else:
        return state
    threading.Thread(target=controller_thread, args=(state, cmd, cwd, env), daemon=True).start()
    return state


# ---------------------------------------------------------------------------
# Game entities
# ---------------------------------------------------------------------------

WEAPON_NAMES = ["normal", "spread", "laser", "missile", "mine", "rapid"]
ARROWS = "→↗↑↖←↙↓↘"


def facing_glyph(angle: float) -> str:
    index = round(angle / (math.pi / 4)) % 8
    return ARROWS[index]


@dataclass
class Bullet:
    x: float
    y: float
    vx: float
    vy: float
    damage: float
    ttl: float
    glyph: str
    homing: bool = False


@dataclass
class Mine:
    x: float
    y: float
    ttl: float = 12.0


@dataclass
class Asteroid:
    x: float
    y: float
    vx: float
    vy: float
    size: int
    hp: float

    @property
    def radius(self) -> float:
        return {3: 1.6, 2: 1.0, 1: 0.5}[self.size]

    @property
    def glyph(self) -> str:
        return {3: "O", 2: "o", 1: "."}[self.size]


@dataclass
class Spark:
    x: float
    y: float
    ttl: float = 0.15


class Game:
    WEAPON_COOLDOWN = {"normal": 0.25, "spread": 0.35, "laser": 0.08, "missile": 0.6, "mine": 1.0, "rapid": 0.08}
    BULLET_SPEED = {"normal": 1.6, "spread": 1.6, "laser": 2.4, "missile": 0.9, "rapid": 2.1}
    THRUST = 6.0
    FRICTION = 0.985
    BOMB_COOLDOWN = 3.0
    HEAVY_COOLDOWN = 1.2

    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.reset()

    def reset(self) -> None:
        self.ship_x = self.width / 2
        self.ship_y = self.height / 2
        self.ship_vx = self.ship_vy = 0.0
        self.angle = -math.pi / 2
        self.lives = 3
        self.score = 0
        self.level = 1
        self.weapon = 0
        self.time = 0.0
        self.invuln_until = 2.0
        self.game_over = False
        self.paused = False
        self.debug = False
        self.bullets: list[Bullet] = []
        self.mines: list[Mine] = []
        self.sparks: list[Spark] = []
        self.asteroids: list[Asteroid] = []
        self._cooldowns: dict[str, float] = {}
        self._prev_buttons: set[int] = set()
        for _ in range(4):
            self._spawn_asteroid()

    def resize(self, width: int, height: int) -> None:
        self.width, self.height = max(width, 20), max(height, 10)

    def _spawn_asteroid(self) -> None:
        edge = random.choice("tblr")
        if edge == "t":
            x, y = random.uniform(0, self.width), 0.0
        elif edge == "b":
            x, y = random.uniform(0, self.width), float(self.height)
        elif edge == "l":
            x, y = 0.0, random.uniform(0, self.height)
        else:
            x, y = float(self.width), random.uniform(0, self.height)
        angle = math.atan2(self.ship_y - y, self.ship_x - x) + random.uniform(-0.9, 0.9)
        speed = random.uniform(0.6, 1.4) * (1.0 + 0.05 * self.level)
        self.asteroids.append(Asteroid(x, y, math.cos(angle) * speed, math.sin(angle) * speed, 3, 3))

    def _ready(self, key: str, cooldown: float) -> bool:
        now = self.time
        if now >= self._cooldowns.get(key, 0.0):
            self._cooldowns[key] = now + cooldown
            return True
        return False

    def _fire(self, kind: str) -> None:
        cooldown = self.WEAPON_COOLDOWN[kind]
        if not self._ready(f"weapon:{kind}", cooldown):
            return
        speed = self.BULLET_SPEED.get(kind, 1.6)
        origin = (self.ship_x, self.ship_y)
        if kind == "spread":
            for offset in (-0.35, 0.0, 0.35):
                a = self.angle + offset
                self.bullets.append(Bullet(*origin, math.cos(a) * speed, math.sin(a) * speed, 1, 1.2, "*"))
        elif kind == "missile":
            self.bullets.append(
                Bullet(*origin, math.cos(self.angle) * speed, math.sin(self.angle) * speed, 3, 2.5, "!", homing=True)
            )
        elif kind == "mine":
            if len(self.mines) < 3:
                self.mines.append(Mine(*origin))
        elif kind == "laser":
            self.bullets.append(
                Bullet(*origin, math.cos(self.angle) * speed, math.sin(self.angle) * speed, 0.5, 0.4, "-")
            )
        else:
            damage = 0.6 if kind == "rapid" else 1.0
            self.bullets.append(
                Bullet(*origin, math.cos(self.angle) * speed, math.sin(self.angle) * speed, damage, 1.2, "*")
            )

    def _bomb(self) -> None:
        if not self._ready("bomb", self.BOMB_COOLDOWN):
            return
        for asteroid in self.asteroids:
            if self._distance(asteroid.x, asteroid.y) < 6.0:
                asteroid.hp -= 5
        self.sparks.append(Spark(self.ship_x, self.ship_y, ttl=0.3))

    def _heavy_shot(self) -> None:
        if not self._ready("heavy", self.HEAVY_COOLDOWN):
            return
        self.bullets.append(
            Bullet(
                self.ship_x, self.ship_y,
                math.cos(self.angle) * 1.2, math.sin(self.angle) * 1.2,
                5, 2.0, "#",
            )
        )

    def _distance(self, x: float, y: float) -> float:
        dx = min(abs(self.ship_x - x), self.width - abs(self.ship_x - x))
        dy = min(abs(self.ship_y - y), self.height - abs(self.ship_y - y))
        return math.hypot(dx, dy)

    def apply_input(
        self,
        x: float,
        y: float,
        held_actions: set[str],
        pressed_actions: set[str],
        dial: int,
        dt: float,
    ) -> None:
        if "pause" in pressed_actions and not self.game_over:
            self.paused = not self.paused
        if "restart" in pressed_actions and self.game_over:
            self.reset()
            return
        if "debug" in pressed_actions and not self.game_over:
            self.debug = not self.debug
        if self.paused or self.game_over:
            return

        self.time += dt

        if dial:
            self.weapon = (self.weapon + dial) % len(WEAPON_NAMES)
        for index in range(6):
            if f"weapon{index}" in pressed_actions:
                self.weapon = index

        magnitude = math.hypot(x, y)
        if magnitude > 0.05:
            self.angle = math.atan2(y, x)
            boost = 1.8 if "boost" in held_actions else 1.0
            self.ship_vx += x * self.THRUST * boost * dt
            self.ship_vy += y * self.THRUST * boost * dt

        self.ship_vx *= self.FRICTION
        self.ship_vy *= self.FRICTION
        self.ship_x = (self.ship_x + self.ship_vx * dt) % self.width
        self.ship_y = (self.ship_y + self.ship_vy * dt) % self.height

        if "fire" in held_actions:
            self._fire(WEAPON_NAMES[self.weapon])
        if "bomb" in held_actions:
            self._bomb()
        if "heavy" in held_actions:
            self._heavy_shot()

        self.shielded = "shield" in held_actions

    def step(self, dt: float) -> None:
        if self.paused or self.game_over:
            return

        for bullet in self.bullets:
            if bullet.homing:
                nearest = self._nearest_asteroid(bullet.x, bullet.y)
                if nearest is not None:
                    target_angle = math.atan2(nearest.y - bullet.y, nearest.x - bullet.x)
                    speed = math.hypot(bullet.vx, bullet.vy) or 0.9
                    current_angle = math.atan2(bullet.vy, bullet.vx)
                    blended = current_angle + _angle_diff(current_angle, target_angle) * 0.15
                    bullet.vx, bullet.vy = math.cos(blended) * speed, math.sin(blended) * speed
            bullet.x = (bullet.x + bullet.vx) % self.width
            bullet.y = (bullet.y + bullet.vy) % self.height
            bullet.ttl -= dt
        self.bullets = [b for b in self.bullets if b.ttl > 0]

        for asteroid in self.asteroids:
            asteroid.x = (asteroid.x + asteroid.vx * dt) % self.width
            asteroid.y = (asteroid.y + asteroid.vy * dt) % self.height

        self._resolve_bullet_hits()
        self._resolve_mines()
        self._resolve_ship_collision()

        for spark in self.sparks:
            spark.ttl -= dt
        self.sparks = [s for s in self.sparks if s.ttl > 0]
        self._maybe_split_and_score()

        if len(self.asteroids) < 3 + self.level and random.random() < 0.02:
            self._spawn_asteroid()
        if self.score >= self.level * 300:
            self.level += 1

    def _nearest_asteroid(self, x: float, y: float) -> Asteroid | None:
        best, best_d = None, math.inf
        for asteroid in self.asteroids:
            d = math.hypot(asteroid.x - x, asteroid.y - y)
            if d < best_d:
                best, best_d = asteroid, d
        return best

    def _resolve_bullet_hits(self) -> None:
        remaining: list[Bullet] = []
        for bullet in self.bullets:
            hit = False
            for asteroid in self.asteroids:
                if math.hypot(bullet.x - asteroid.x, bullet.y - asteroid.y) < asteroid.radius + 0.3:
                    asteroid.hp -= bullet.damage
                    self.sparks.append(Spark(bullet.x, bullet.y))
                    hit = True
                    break
            if not hit:
                remaining.append(bullet)
        self.bullets = remaining

    def _resolve_mines(self) -> None:
        remaining: list[Mine] = []
        for mine in self.mines:
            mine.ttl -= 0.05
            triggered = False
            for asteroid in self.asteroids:
                if math.hypot(mine.x - asteroid.x, mine.y - asteroid.y) < asteroid.radius + 1.2:
                    asteroid.hp -= 4
                    triggered = True
            if triggered:
                self.sparks.append(Spark(mine.x, mine.y, ttl=0.3))
            elif mine.ttl > 0:
                remaining.append(mine)
        self.mines = remaining

    def _resolve_ship_collision(self) -> None:
        now = self.time
        if now < self.invuln_until or getattr(self, "shielded", False):
            return
        for asteroid in self.asteroids:
            if math.hypot(self.ship_x - asteroid.x, self.ship_y - asteroid.y) < asteroid.radius + 0.6:
                self.lives -= 1
                self.sparks.append(Spark(self.ship_x, self.ship_y, ttl=0.4))
                self.ship_x, self.ship_y = self.width / 2, self.height / 2
                self.ship_vx = self.ship_vy = 0.0
                self.invuln_until = now + 2.0
                if self.lives < 0:
                    self.game_over = True
                break

    def _maybe_split_and_score(self) -> None:
        survivors: list[Asteroid] = []
        for asteroid in self.asteroids:
            if asteroid.hp > 0:
                survivors.append(asteroid)
                continue
            self.score += (4 - asteroid.size) * 10
            if asteroid.size > 1:
                for _ in range(2):
                    angle = random.uniform(0, math.tau)
                    speed = random.uniform(0.8, 1.6)
                    survivors.append(
                        Asteroid(
                            asteroid.x, asteroid.y,
                            math.cos(angle) * speed, math.sin(angle) * speed,
                            asteroid.size - 1, asteroid.size - 1,
                        )
                    )
        self.asteroids = survivors


def _angle_diff(a: float, b: float) -> float:
    diff = (b - a + math.pi) % math.tau - math.pi
    return diff


# ---------------------------------------------------------------------------
# Input translation (device buttons / keyboard -> logical actions)
# ---------------------------------------------------------------------------

KEY_HELD = {
    ord("f"): "boost",
    ord("g"): "shield",
}
KEY_PRESSED = {
    ord("b"): "bomb-once",
    ord("p"): "pause",
    ord("r"): "restart",
    ord("["): "dial-left",
    ord("]"): "dial-right",
}


def device_actions(buttons: set[int]) -> tuple[set[str], set[str]]:
    held: set[str] = set()
    for index in buttons:
        if index in WEAPON_BUTTONS:
            held.add(f"weapon{index}")
        elif index == BOMB_BUTTON:
            held.add("bomb")
        elif index == BOOST_BUTTON:
            held.add("boost")
        elif index == HEAVY_BUTTON:
            held.add("heavy")
        elif index == SHIELD_BUTTON:
            held.add("shield")
        elif index == FIRE_BUTTON:
            held.add("fire")
    return held, set()


# ---------------------------------------------------------------------------
# Curses front end
# ---------------------------------------------------------------------------

def run(stdscr: "curses.window", controller: InputState) -> None:
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(0)
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()

    height, width = stdscr.getmaxyx()
    game = Game(max(width - 1, 20), max(height - 3, 10))

    kb_x = kb_y = 0.0
    kb_last_move = 0.0
    prev_device_buttons: set[int] = set()
    tick = 0.05  # 20 Hz

    while True:
        loop_start = time.monotonic()
        keyboard_pressed: set[str] = set()
        keyboard_held: set[str] = set()
        moved_this_tick = False
        while True:
            key = stdscr.getch()
            if key == -1:
                break
            if key in (ord("q"), 27):
                return
            if key in (curses.KEY_UP, ord("w")):
                kb_x, kb_y = 0.0, -1.0
                moved_this_tick = True
            elif key in (curses.KEY_DOWN, ord("s")):
                kb_x, kb_y = 0.0, 1.0
                moved_this_tick = True
            elif key in (curses.KEY_LEFT, ord("a")):
                kb_x, kb_y = -1.0, 0.0
                moved_this_tick = True
            elif key in (curses.KEY_RIGHT, ord("d")):
                kb_x, kb_y = 1.0, 0.0
                moved_this_tick = True
            elif key == ord(" "):
                keyboard_held.add("fire")
            elif ord("1") <= key <= ord("6"):
                keyboard_pressed.add(f"weapon{key - ord('1')}")
            elif key in KEY_HELD:
                keyboard_held.add(KEY_HELD[key])
            elif key in KEY_PRESSED:
                action = KEY_PRESSED[key]
                if action == "bomb-once":
                    keyboard_held.add("bomb")
                elif action == "dial-left":
                    game.weapon = (game.weapon - 1) % len(WEAPON_NAMES)
                elif action == "dial-right":
                    game.weapon = (game.weapon + 1) % len(WEAPON_NAMES)
                else:
                    keyboard_pressed.add(action)

        if moved_this_tick:
            kb_last_move = loop_start
        elif loop_start - kb_last_move > 0.2:
            kb_x *= 0.6
            kb_y *= 0.6

        dev_x, dev_y, dev_buttons, dial, connected = controller.snapshot()
        new_device_presses = dev_buttons - prev_device_buttons
        prev_device_buttons = dev_buttons

        device_held, _ = device_actions(dev_buttons)
        device_pressed: set[str] = set()
        if PAUSE_BUTTON in new_device_presses:
            device_pressed.add("pause")
        if MENU_BUTTON in new_device_presses:
            device_pressed.add("restart" if game.game_over else "debug")

        use_device = connected and (abs(dev_x) + abs(dev_y) > 0.02 or bool(dev_buttons))
        input_x, input_y = (dev_x, dev_y) if connected else (kb_x, kb_y)
        held_actions = device_held | keyboard_held
        pressed_actions = device_pressed | keyboard_pressed

        game.apply_input(input_x, input_y, held_actions, pressed_actions, dial, tick)
        game.step(tick)

        render(stdscr, game, connected, use_device)

        elapsed = time.monotonic() - loop_start
        if elapsed < tick:
            time.sleep(tick - elapsed)


def render(stdscr: "curses.window", game: Game, connected: bool, use_device: bool) -> None:
    stdscr.erase()
    max_y, max_x = stdscr.getmaxyx()
    game.resize(max_x - 1, max_y - 3)

    status = "DEVICE" if connected else "KEYBOARD"
    header = (
        f" score {game.score:5d}  lives {max(game.lives, 0)}  "
        f"weapon {WEAPON_NAMES[game.weapon]:<7} level {game.level}  [{status}]"
    )
    if game.paused:
        header += "  -- PAUSED --"
    _safe_addstr(stdscr, 0, 0, header[: max_x - 1])

    for asteroid in game.asteroids:
        _safe_addstr(stdscr, int(asteroid.y) + 1, int(asteroid.x), asteroid.glyph)
    for bullet in game.bullets:
        _safe_addstr(stdscr, int(bullet.y) + 1, int(bullet.x), bullet.glyph)
    for mine in game.mines:
        _safe_addstr(stdscr, int(mine.y) + 1, int(mine.x), "x")
    for spark in game.sparks:
        _safe_addstr(stdscr, int(spark.y) + 1, int(spark.x), "+")

    ship_glyph = facing_glyph(game.angle)
    if game.time < game.invuln_until:
        ship_glyph = ship_glyph.lower() if ship_glyph.isalpha() else ship_glyph
    _safe_addstr(stdscr, int(game.ship_y) + 1, int(game.ship_x), ship_glyph)

    footer = "arrows/stick=move  space/trigger=fire  1-6/AG=weapon  b/lab=bomb  f/yolo=boost  g/magic=shield  p/mic=pause  q=quit"
    _safe_addstr(stdscr, max_y - 1, 0, footer[: max_x - 1])

    if game.debug:
        debug_line = f"input={'device' if use_device else 'keyboard'} angle={math.degrees(game.angle):.0f}"
        _safe_addstr(stdscr, max_y - 2, 0, debug_line[: max_x - 1])

    if game.game_over:
        message = f" GAME OVER -- score {game.score} -- press r / menu-grid to restart "
        _safe_addstr(stdscr, max_y // 2, max(0, (max_x - len(message)) // 2), message)

    stdscr.refresh()


def _safe_addstr(stdscr: "curses.window", y: int, x: int, text: str) -> None:
    max_y, max_x = stdscr.getmaxyx()
    if 0 <= y < max_y and 0 <= x < max_x:
        try:
            stdscr.addstr(y, x, text[: max_x - x - 1] if x < max_x - 1 else "")
        except curses.error:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe-dir", default=str(DEFAULT_PROBE_DIR), help="codex-micro-analysis checkout directory")
    parser.add_argument("--controller-cmd", help="override the command used to stream gamepad-state JSONL")
    parser.add_argument("--keyboard-only", action="store_true", help="skip the device feed entirely")
    args = parser.parse_args()

    if args.keyboard_only:
        controller = InputState()
    else:
        probe_dir = Path(args.probe_dir).expanduser()
        controller = start_controller(probe_dir, args.controller_cmd)

    try:
        curses.wrapper(run, controller)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
