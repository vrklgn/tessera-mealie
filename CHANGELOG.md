# Changelog

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
