# Playing a DS game through the emulator harness

You are playing a Nintendo DS game in an emulator to reach many different scenes. Each scene you
reach becomes a test point for a save-state feature. You don't need to win or play well. What
matters is variety: different areas, battles, menus, cutscenes, 3D scenes and minigames.

## The tool

Everything goes through one command, which you run with Bash:

    python3 tools/rts-harness/host/play.py <command> <session> ...

- `new SESSION GAME` boots the game. It prints a screenshot path.
- `step SESSION "SCRIPT"` presses the inputs in SCRIPT, then prints the screenshot path(s) and
  the frame number. Each step takes 1–3 seconds.
- `undo SESSION [N]` goes back N steps. Use it if you got stuck, died, or picked the wrong option.
- `mark SESSION LABEL` keeps the current moment as a checkpoint. This is your main output.
- `log SESSION` lists the steps so far.
- `exportsave SESSION $PLAYROOT/saves/NAME.sav` copies out the game's own save file.
  Do this after you save in the game.

**Always look at the printed screenshot with the Read tool after each step.**
- The image is 512x768: the top screen is the upper half and the touch screen is the lower half.
- Touch coordinates are in DS pixels, 0–255 by 0–191, measured on the bottom screen only. So a
  point in the image at (px, py), with py ≥ 384, is touch x = px/2, y = (py-384)/2.

## SCRIPT tokens (space separated, run in order; 60 frames = 1 second)

| Token | Meaning |
|---|---|
| `A` `B` `X` `Y` `L` `R` `START` `SELECT` `UP` `DOWN` `LEFT` `RIGHT` | tap the button |
| `RIGHT:60` | hold for 60 frames (walking uses this) |
| `UP+A:30` | hold buttons together |
| `w:60` | wait 60 frames |
| `mash:A:10` | tap A ten times (skips dialogue quickly) |
| `t:128,96` | touch the touch screen for 20 frames; `t:128,96:40` holds it for 40 |
| `drag:40,100,200,100:30` | drag across the touch screen over 30 frames |
| `path:40,40,200,40,200,150,40,150,40,40:10` | one continuous stroke through the points (draws a closed loop: circles, puzzle answers) |
| `snap` | take an extra screenshot at that point |

Keep each step short: 1–8 seconds of game time, so you can see what happened. For text-heavy
parts, `mash:A:15` per step is fine. Check that the scene actually moved on.

## What to do

1. `new <session> <game>`, then look at the screen.
2. Get through the title and the menus. If there's a "Continue" with existing save data, it's
   fine to use it. Otherwise start a new game.
3. Play forward. Each time you reach a scene that looks clearly different, `mark` it with a short
   label such as `field_town`, `battle_first`, `menu_items`, `cutscene_intro`, `map_world` or
   `boss`. Mark during live action, not on a black or fading screen. Aim for 8–15 checkpoints
   across as many kinds of scene as possible.
4. If the game lets you save, save at least once after making real progress. Then run
   `exportsave` to `$PLAYROOT/saves/<game>_<label>.sav`.
5. Stuck for more than about 8 steps with nothing changing: try other buttons, START, the touch
   screen, or `undo` a few steps and try something else.
6. Stop after about 120 steps, or once you've got good variety.

## Lessons from earlier players

- Mashing A on a keyboard or name-entry screen types letters. Use touch or the D-pad there instead.
- Mashing A at a yes/no question picks the default, which can loop a tutorial. Read the question first.
- To leave a room, look for stairs, a doormat or a dark gap in the wall, and walk into it. In
  Pokémon, the stairs in the player's bedroom are in the top-right corner.
- Black or blank screens during transitions are normal. Wait 60–120 frames before deciding
  something is wrong.

## Rules

- `undo` only reaches back about 40 steps. To go further, use a checkpoint. NEVER run `new` on a session that already exists: it refuses, because it would wipe every checkpoint.
- Use only play.py. Don't edit any files. Don't touch other sessions.
- Finish with a short report:
  - each checkpoint label with a one-line description of the scene;
  - any exported saves;
  - anything odd you saw, such as graphics glitches, freezes, or garbled or black screens that
    didn't recover. These matter: the emulator is running a custom loader, so report them
    precisely with the step number.
