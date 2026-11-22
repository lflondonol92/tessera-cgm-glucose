# Changelog

## 0.2.0
- The tile now lays itself out like SugarTV Card: a wide tile is a row (time, reading, arrow, change), any other a
  column, and the reading takes the largest size that fits.
- The line over the reading is now the clock time of the reading, as in SugarTV Card. A new option, **Time and
  forecast**, brings back "how long ago" and can switch off the forecast.
- New forecast line under the reading (the trend arrow in words) and the active insulin line moved above the reading,
  both when the tile is tall enough.
- A clock shows in place of the change while none is known, and a question mark when the trend sensor says something the
  tile does not know.
- A glucose sensor with no unit is no longer taken for mg/dL when it reports millimoles: a 7.9 was shown as an urgent
  low. It is now told apart by its number, and any spelling of the unit that says mmol is recognised.
- The four limits now stand in order whatever you set, so a Low under the Urgent low can no longer hide a zone.

## 0.1.0
- First version: the reading, a trend arrow, the change, the age and the active insulin, from the sensors you choose.
- Trend names for Medtronic Carelink, Dexcom, Nightscout, LibreView, LibreLink, LibreLinkUp, Medtrum and others, or a
  rate of change per minute.
