# CGM glucose

A tile for a continuous glucose monitor: the reading, a trend arrow, the change since the last reading and how old it
is. It reads the sensors you choose in Home Assistant, so it works with any integration that publishes them: Medtronic
Carelink, Dexcom, Nightscout, LibreLinkUp and the others [SugarTV Card](https://github.com/wiltodelta/homeassistant-sugartv-card)
supports.

> This tile is a convenience display. Do not take treatment decisions from it. A reading can be late, and the tile
> greys the number when it is older than you chose, but it cannot know what your sensor or app know.

## Set up

1. Add the plugin to a screen in Tessera. The screen builds once.
2. In Layout, place the **Glucose** tile from the library's Plugins group.
3. In the inspector, choose the **Glucose sensor**. That is all the tile needs.
4. Choose the other sensors your integration has. None of them is required:
   - **Trend sensor**: the arrow. If your integration keeps the trend in an attribute of the glucose sensor (Nightscout's
     `direction`, LibreView's `trend`), leave this empty.
   - **Trend values**: how to read the trend sensor. Words or codes suits most; **Carelink** is for Medtronic, where
     `NONE` means steady; **Rate of change** is for a sensor in mg/dL or mmol/L per minute.
   - **Change sensor**: the change since the previous reading.
   - **Reading time sensor**: when the reading was taken, so the age is true.
   - **Active insulin sensor**: shows the insulin on board when the tile is tall enough.
5. Adjust the limits if yours are not the standard 54, 70, 180 and 250 mg/dL.

### Medtronic (Carelink)

With the Carelink integration, choose these (the names start with your own prefix):

| Option | Sensor |
|---|---|
| Glucose sensor | `Last glucose level mg/dl` (or `... mmol`) |
| Trend sensor | `Last glucose trend`, with **Trend values** on Carelink |
| Change sensor | `Last glucose delta` |
| Reading time sensor | `Last glucose update` |
| Active insulin sensor | `Active insulin` |

## Good to know

- Zones follow the international consensus ranges: red for urgent low (under 54) and urgent high (over 250), amber for
  low and high, normal text in range. A sensor in mmol/L is converted before it is compared, so the limits are always
  in mg/dL.
- The arrows are the seven of a CGM. The screen has icons for the single arrows; a quick rise or fall draws two. A
  trend the tile does not know draws no arrow.
- The age counts from the reading time sensor. Without one, it counts from the moment Home Assistant last heard from the
  glucose sensor, which can be a few minutes after the reading.
- A reading older than **Old reading after** (15 minutes by default) is greyed, its change is hidden and its age turns
  amber.
- The tile needs the screen's clock to show an age. Until Home Assistant sets it, no age is shown.
- Without a change sensor, the tile works the change out itself, but only when it has seen two different readings less
  than nine minutes apart. A screen that was off, or on another page, has not.
- The trend names, the time rules and the standard limits follow SugarTV Card (MIT). See `NOTICE`.
