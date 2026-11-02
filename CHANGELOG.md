# Changelog

## 0.1.1 - 2026-11-02
- A recipe picked on a phone (the package's select on a Home Assistant dashboard) shows on the screen: an open
  recipe card follows the select after a pick on the picker too, and drops the last recipe's picture.
- The packages make `input_number.tessera_mealie_step`, the step a steps tile follows, and set it back to 1 when
  another recipe is chosen; an optional line opens the recipe's page on a screen.
- A steps tile that counts for itself starts a new recipe at its first step.

## 0.1.0 - 2026-10-31
- First version: the meal planned in Mealie on a tile, from a Mealie calendar of the Mealie integration or from a
  meal sensor the plugin's Home Assistant package makes straight from Mealie's API.
- The meal's picture on the tile and the card, through an image entity of Home Assistant.
- A tap opens the recipe: its facts, its ingredients and its steps, page by page, from a Home Assistant script
  the plugin's package brings.
- "Dinner: Vodkapasta" in the top bar on the day of the meal.
- A second tile, Chosen recipe, for a recipe picked by hand from a list on the screen: a select entity in Home
  Assistant, which the package fills with every recipe in Mealie.
- Both tiles can show the ingredients, or one step at a time, large, on the page itself: no tap needed. The
  ingredients turn their page every so many seconds; the step follows a number entity, which a tap, voice or an
  automation moves on.
