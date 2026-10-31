# Mealie

The meal planned in [Mealie](https://mealie.io) on a Tessera tile: what is for dinner tonight (or the next day
something is planned), its picture, the servings and the time it takes. A tap opens the recipe: its ingredients and
its steps, page by page, written so the screen never cuts a line in half. On the day of the meal the top bar can say
"Dinner: Vodkapasta".

The tile belongs to a Mealie calendar of Home Assistant's [Mealie integration](https://www.home-assistant.io/integrations/mealie/)
(`calendar.mealie_dinner`, one per meal type), so it works the moment the plugin is on a screen. The recipe itself,
and the picture, come through a small Home Assistant package the plugin brings (`homeassistant/`): a script the
screen asks page by page, and an image entity with the meal's picture. Two packages do the same job, choose one:

| Package | Reads Mealie through | For |
|---|---|---|
| `homeassistant/tessera_mealie.yaml` | The Mealie integration's own actions (`mealie.get_mealplan`, `mealie.get_recipe`) | Homes with the Mealie integration. Nothing to fill in but your Mealie's address, for the pictures. |
| `homeassistant/tessera_mealie_api.yaml` | Mealie's API directly, with your Mealie's address and an API token in `secrets.yaml` | Homes without the integration, or that want the plugin to read Mealie directly. It also makes the meal sensor the tile belongs to. |

> A Tessera screen never reaches Mealie, and the Tessera app never reaches your home network on a plugin's behalf:
> that is how Tessera is built (its plugins' `docs/LIMITS.md`). So the address of your Mealie and its API token
> live in Home Assistant, which does the asking, and the plugin asks Home Assistant. Nothing leaves the house.

## Set up

1. In Tessera, open **Plugins**, choose **Mealie** and tick your screens. It asks which calendar the top bar should
   follow (`calendar.mealie_dinner`); with the API package, choose `sensor.tessera_mealie_dinner` instead. Each screen
   builds once.
2. In **Layout**, place the **Meal** tile from the library's Plugins group, at any size from 1x1 to 4x4, and choose
   its calendar in the inspector: `calendar.mealie_dinner`, or `calendar.mealie_lunch` for a second tile. The tile now
   shows the meal, its description and the day. Give the tile a name ("Dinner") or it takes the word for the meal type
   from the calendar's id.
3. For the picture, the servings and the time, and the recipe on a tap, install one package in Home Assistant:
   copy `homeassistant/tessera_mealie.yaml` (or `tessera_mealie_api.yaml`) to `/config/packages/`, follow the few
   lines at the top of the file (your Mealie's address; for the API package, a token in `secrets.yaml`), and reload
   **All YAML configuration** in Developer tools. Home Assistant 2025.4 or newer. If `configuration.yaml` has no
   `packages` line yet:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

4. Back in the tile's inspector, choose `image.tessera_mealie_dinner` under **Picture**. The tile shows the picture
   from the next time its data comes; the card shows it large.
5. Optional: place **Today's meal** on a page's top bar (under "From plugins").

More meal types: the integration package makes images for dinner and lunch; copy an `image` block for breakfast, side,
dessert, snack or drink and change the calendar in it. The API package makes the dinner sensor and image; copy both
blocks and change `dinner` to the type you want.

## On the screen

- **The tile.** The meal type and the day at the top ("Dinner", "Today", "Tomorrow", "Fri 9 Oct"), the recipe's name
  large, its description where there is room, the servings and the time at the bottom. On a tile two columns or more
  wide and two rows or more high the picture lies across the top; on a flat tile it stands at the left; a single cell
  has no picture. A board without pictures (the CYD) shows the words alone.
- **The card.** The picture, the name, the day, the servings and the time, the description, then the ingredients. A
  tap turns the page: the rest of the ingredients, then the steps, numbered, then the ingredients again. A section's
  name ("Optional: home-made pasta") stands in bold over its lines. Back closes it.
- **The top bar.** "Dinner: Vodkapasta" on the day of the meal, from the calendar chosen when the plugin was added.
- **While it waits.** "Choose a Mealie calendar" until the tile has an entity, "Nothing planned" for a calendar with
  no meal in the coming week, "Mealie is away" while Home Assistant has no value. The card says "Asking Mealie" while
  a page is on its way, and says so when the script is not installed or the recipe is not in Mealie.
- **Options** in the inspector: **Picture** (the image entity), **Ask for the recipe** (on; off keeps the tile to
  what the calendar says and never asks the script) and **Show the description** (on).

## How it works

- `tessera-plugin.yaml`: one tile of a `calendar` or `sensor` entity (`message`, `description`, `start_time`), a card
  with a picture, a top bar item, and one Home Assistant command the app may ask on the plugin's behalf:
  `call_service:script.tessera_mealie`.
- `components/mealie/`: the ESPHome component. The tile reads its entity, builds the picture's address through Home
  Assistant's image proxy (`/api/image_proxy/<image entity>`), and asks the script once per meal for its facts. The
  card asks the script for one page at a time, as many lines as fit and as many characters as its width holds.
- `homeassistant/`: the script and the entities, above.
- `translations/`: the words on the screen (`screen`, English and Swedish) and in the app (`app`).

**The script's contract**, so you can write a backend of your own (any script named `script.tessera_mealie` that
answers like this works, whatever it reads):

| Asked with | Answers |
|---|---|
| `recipe` (the meal's name, or a slug), `date` (`2026-10-31`, optional), `part: info` | `{ ok: true, name, slug, servings (a number), yield (Mealie's word for it, or ""), total, prep, cook (texts, or ""), ingredients, steps (counts), picture (the recipe id) }` |
| the same with `part: ingredients` or `steps`, `page` (from 1), `per` (lines a page), `cols` (characters a line, at most 46) | `{ ok: true, part, page, pages, lines: [...] }`: `per` lines of at most `cols` characters and 46 bytes, wrapped on words. A line that goes on from the one before starts with a space; a section's name starts with `# `; a step's first line starts with its number, `3. `. |
| a recipe it cannot find | `{ ok: false, error: not_found }` |

The script finds the recipe on the meal plan of `date` under that name (so two recipes with one name do not mix),
else by searching Mealie for the name.

## Good to know

- Tessera carries at most 48 bytes of any text and about 3 KB of any answer, which is why the recipe comes as wrapped
  lines, a page at a time, and why the tile's description is its first 48 bytes. The script wraps to the card's own
  width, so a page is never cut with dots; a word longer than a line is.
- The Mealie calendars show the next planned meal of their type within a week, so a tile shows tomorrow's dinner once
  today's is past (Mealie's calendar keeps today's meal for the whole day).
- The pictures are fetched by Home Assistant from your Mealie (its pictures need no login) and reach the screen through
  Home Assistant's own image proxy; a new picture is a new address, so a tile never shows the last meal's picture.
- The plugin has no `store.png` and has not been run on the bench or a screen yet (see `AGENTS.md` for both). Try it
  on your own screen first; `TESTING.md` of the plugins repository has the three ways.
- A tile cannot take a whole page; any other size of the grid is fine.

## Privacy

Nothing goes outside your home: the plugin names no host on the internet, and the app asks only Home Assistant. The
script is logged by the app as every command a plugin asks (`permissions.ha_commands`).
