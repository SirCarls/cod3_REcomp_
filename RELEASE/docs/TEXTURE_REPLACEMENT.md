# Replacing textures with your own pictures

The recompiled Call of Duty 3 can draw any of the game's textures with a
picture you make. This is meant first for the swastikas and other Nazi
insignia (the full list is below), but it works for any texture. Your
pictures go into a **user texture pack**, which the setup tool builds and the
game reads after the HD texture pack, so your picture wins over the upscaled
one. Nothing is downloaded and nothing of the game's own files is changed.

## Quick start

1. Build once with the setup tool, so the game's textures are ripped to
   `<game folder>/data/cod3/textures/` (each as a PNG, plus `hd_keys.tsv`).
2. On the **Textures** page, press **Open picture folder**. It opens (and
   creates) `<game folder>/data/cod3/user_textures_png/`, with a copy of
   `insignia_textures.txt`, the list of insignia textures.
3. Copy the ripped original you want to replace from `data/cod3/textures/`,
   for example `cloth_masked$of_flag_german.png`, edit it, and save it in
   `user_textures_png/` under **the same name**.
4. Press **Build user texture pack**. The Log page says what was packed and,
   for each picture that broke a rule, why it was skipped. The pack is
   `<game folder>/data/cod3/user_textures/`.
5. Start the game. Your pictures are used from that start on.

A normal **Build** also builds the pack when the folder has pictures.

## The rules for your pictures

| What | Rule |
|---|---|
| File type | PNG. 8 bits per channel: RGBA (best), RGB, gray, or gray + alpha. |
| Not allowed | palette (indexed-color) PNGs, 16-bit PNGs, interlaced PNGs, animated PNGs (APNG). Save as "RGBA, 8 bits per channel, no interlacing". |
| Name | Exactly the ripped picture's name (the `file` column of `insignia_textures.txt`, or the PNG's name in `data/cod3/textures/`), `$` and all, case and all, with `.png`. A few names hold more than one picture; the others are saved as `<name>__<16 hex digits>.png`, and you replace each under its own name. |
| Aspect ratio | The original's (to within a texel of rounding). |
| Size | At least the original's size, at most 8192 a side, any scale (1x, 2x, 3x, 4x, 2.5x ...). |
| Block size | The game keeps most textures compressed in 4 x 4 blocks (DXT1, DXT3, DXT5, ATI2), so both sides must be multiples of 4. A whole scale of the insignia below always is. |
| File size | At most 256 MB. |
| Alpha | Never more see-through than the original. If the original is **opaque**, your picture's alpha is ignored and it is drawn opaque. If the original has **transparent parts** (a torn banner edge, say), each texel of your picture keeps at least the original's alpha: you may fill holes, never cut new ones. A PNG without alpha takes the original's alpha. |
| Format | You don't choose it: your picture is stored in the original's format (DXT1, DXT3, DXT5, ATI2 or 32-bit) with all its mip levels. |

Lightmaps (`lm0001` ...), terrain color lightmaps, fog ramps and other
lookup tables (8 texels or fewer on a side) hold lighting data, not
pictures, and can't be replaced.

Tip: start from the ripped original and paint over only the symbol, so the
picture still lines up with the model's shape (for a helmet, the decal sits
on a small patch of a texture that wraps the whole helmet).

## Checking that it works

- The setup tool's log lists `packed <name>.dds (2x)` for each picture, and
  `skipped: <name>.png: <reason>` for the others.
- `rip cod3-check-pack <game folder>/data/cod3/user_textures` checks a
  pack with the game's own rules (useful for a pack someone else made).
- In the game, start it with `--texture_replacement_log=true` (or set
  `texture_replacement_log = true` in `cod3.toml`). The log
  (`userdata/logs/`) then has one line for every texture that could be
  replaced, when the game first draws it, for example:

  ```text
  Texture replacement: e26a0a1331259fd8 512x256 format 18 at 0x1A2B3000: replaced with .../user_textures/cloth_masked$of_flag_german.dds (1024x512, 2.00x, 10 levels, user pack)
  ```

  The first number is the texture's key; it is the same as in
  `insignia_textures.txt`. A picture the game refuses is named once in a
  `Texture replacement: can't use ...` line with the reason.

## Multiplayer

In a multiplayer match no texture change may give anyone an edge: a replaced
wall, bush or uniform could be made see-through or bright to spot players.
So while a match runs:

- **Only the world insignia** on the game's own list are replaced from your
  pack: the flags, banners, the Fw 190's markings, the crate stamp, the knife
  grip, the poster and the two menu pictures (`mp` = `yes` in the table).
  The list is built into the game (`recomp/src/game/hd_textures.cpp`), so
  editing a data file doesn't widen it. The world insignia found in the
  other levels (a banner, the war flag, crates, posters, a knife, the Stuka)
  aren't on it yet, so a match shows them as the game has them.
- **Everything else is the game's own**: the HD textures, any other picture
  in your pack, and the insignia that soldiers or vehicles wear (helmets,
  uniforms, tanks), which show as the game has them during the match.
- Even the allowed pictures are checked against the original's alpha when
  the game loads them, so they can't be more see-through than the original.

The change is live: when a match starts, the game turns this on
(`texture_replacement_allow_list`), textures already drawn are made again at
their next use, and when the match ends everything is back as before.

## What the game checks (safety)

The packs are files anyone could edit, so the game treats everything in them
as untrusted:

- `index.tsv` may only name files inside the pack folder: no absolute paths,
  no `..`, no backslashes, drive letters or `:`, no control characters, at
  most 255 bytes, and the file opened (symbolic links followed) must be
  inside the folder. The index (64 MB at most) is read once at start; no
  folder is scanned.
- Every DDS file's header is checked before anything is read or allocated:
  a plain DDS header (no DX10 header, no cube map or volume), the format the
  index says, sides from 1 to 16384 and no larger than the GPU allows, at
  most 15 mip levels and no more than the image can have, the original's
  aspect ratio, and a file length exactly matching the levels. The sizes come
  from the checked header, never from the index.
- All replacement images together may take at most
  `texture_replacement_memory_limit` MB (4096 by default); beyond that, the
  game's own textures are used.
- A file that fails any check is skipped with one log line; the game never
  stops for it.
- The setup tool's builder checks your pictures (above), and checks the pack
  it wrote with the same rules as the game.

Settings (in `cod3.toml` or on the command line): `cod3_user_textures`
(use the user pack, default on), `texture_replacement_enabled` (all
replacement on or off, live), `texture_replacement_memory_limit`,
`texture_replacement_log`.

## The insignia textures

Found in all 15 single-player levels, the shared archives and the menus:
first by name, then by looking at every candidate picture, then from the
materials that use each one. Saint-Lô (the first level) and the shared
archives were checked in the archives themselves, the other 14 levels in
their ripped textures and material listings. The multiplayer maps weren't
checked (their archives weren't available); a picture they share with the
single-player levels has the same key and is covered here. The movies are
video, not textures, and can't be replaced this way.

**kind**: *party* = a Nazi party symbol (swastika, SS runes, party eagle);
*military* = a German military marking (Balkenkreuz, Iron Cross), not a party
symbol; *related* = Nazi-era, no party symbol visible.

### Saint-Lô, the shared archives and the menus

| file (your PNG's name) | key | original | sizes (whole scales) | alpha | in MP | kind | shows |
|---|---|---|---|---|---|---|---|
| `cloth_masked$of_flag_german` | e26a0a1331259fd8 | 512x256 DXT1 | 512x256, 1024x512, 2048x1024, 4096x2048 | opaque | yes | party | Party flag: a black swastika on a white disc, red field |
| `decal$of_germanflag_dirty_c` | 3d71bea5f9c2267d | 256x512 DXT1 | 256x512, 512x1024, 1024x2048, 2048x4096 | transparent parts | yes | party | Hanging party banner with a swastika |
| `of_c_pnzgren_helm1_c` | d7d10142dfca1784 | 256x512 DXT1 | 256x512, 512x1024, 1024x2048, 2048x4096 | opaque | no | party | Soldier's helmet: SS runes and swastika shield decals |
| `of_c_pnzgren_helm2_c` | 8d5ba713bcfd5f25 | 256x512 DXT1 | as above | opaque | no | party | The same helmet with a net |
| `of_c_pnzgren_helm3_c` | bec9817db2754aff | 256x512 DXT1 | as above | opaque | no | party | The same helmet, plain |
| `of_c_pnzgren_body1_c` | 7d8c6dbbfd2117ce | 1024x1024 DXT1 | 1024x1024, 2048x2048, 4096x4096, 8192x8192 | opaque | no | party | Panzergrenadier uniform: SS runes collar tab (and an eagle belt buckle) |
| `c_pnzgren_body1_s` | 04aca89141fa9c11 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | party | The uniform's shine map: the collar runes show faintly (optional) |
| `metal$of_v_fw190` | cc05fcb9517dacb0 | 512x256 DXT1 | 512x256, 1024x512, 2048x1024, 4096x2048 | opaque | yes | party | Fw 190 fighter: tail-fin swastika; Balkenkreuz crosses |
| `wood$us_gersmallcrate_top_c` | 78110e521ebffb27 | 256x256 DXT1 | 256x256 ... 2048x2048 | opaque | yes | party | Crate lid ink stamp: an eagle over a wreath |
| `metal$p_ger_knife_c` | 244cd5634547c2be | 512x64 DXT1 | 512x64, 1024x128, 2048x256, 4096x512 | opaque | yes | related | Hitler Youth knife grip diamond (center blank) |
| `decal$us_german_propaganda_` | 43e8a8f26b93615f | 256x256 DXT3 | 256x256 ... 2048x2048 | transparent parts | yes | related | Propaganda poster, no party symbol |
| `panzezriv_c` | aede510890dfb141 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Panzer IV tank: Balkenkreuz crosses |
| `sp_bs_veh_main_icon_02` | fe2665c6dacb3f45 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | yes | military | Bonus menu: Iron Cross on red behind a tank |
| `sp_bs_wpn_main_icon_02` | 2beac860929168f5 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | yes | military | Bonus menu: the same cross behind a P38 pistol |

*in MP* is *no* for every picture found in the other levels for now: the
game's own multiplayer list doesn't have their keys yet (see Multiplayer).

### The other levels: world insignia

Flags, crates, posters, a knife and the Stuka.

| file (your PNG's name) | key | original | sizes (whole scales) | alpha | in MP | kind | shows |
|---|---|---|---|---|---|---|---|
| `cloth$of_germanflag_dirty_c` | d616f2029e2f1500 | 256x512 DXT1 | 256x512 ... 2048x4096 | transparent parts | no | party | Hanging party banner (cloth version): a swastika on a white disc, dirty red cloth |
| `wood$of_crate_top_axis_c` | 78f187697b49bea1 | 256x256 DXT1 | 256x256 ... 2048x2048 | opaque | no | party | Crate lid stencil: an eagle with a swastika in its wreath, "Deutsches Wehrmacht", a small eagle stamp and "Der Reichsfuehrer-SS" with SS runes |
| `wood$crate_top_axis_c` | 6fd792844e53aa72 | 256x256 DXT1 | 256x256 ... 2048x2048 | opaque | no | party | The same crate lid, edited: the eagle over an empty wreath (no swastika), a small eagle stamp |
| `of_knife148_c` | f7b23274ceea0665 | 512x128 DXT1 | 512x128 ... 4096x1024 | opaque | no | party | Hitler Youth knife: the red and white diamond emblem with a swastika on the grip, "Blut und Ehre" on the blade |
| `decal$german_propaganda1_c` | cfcee273e57f1cea | 256x256 DXT3 | 256x256 ... 2048x2048 | transparent parts | no | related | Wartime propaganda poster: a Hitler Youth boy with an armband; no legible symbol |
| `decal$german_propaganda5_c` | 8fe7a1b07761ef73 | 256x256 DXT3 | 256x256 ... 2048x2048 | transparent parts | no | related | Wartime propaganda leaflet "Longing for You" (a pin-up); no symbol |
| `cloth$germankriegsflag_dirt` | 87c95b90af7d844a | 256x512 DXT1 | 256x512 ... 2048x4096 | transparent parts | no | military | German war flag as the game has it: an Iron Cross in the center disc (where the real flag has a swastika) |
| `metal$stuka_body` | 4f951004958dffc5 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | military | Stuka (Ju 87) dive bomber in flight: Balkenkreuz crosses; no swastika in this picture |
| `metal$stuka_wingtip_c` | fb61d0649fcd34a3 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | military | A Stuka's wrecked wing: a Balkenkreuz |

### The other levels: soldiers (uniforms, helmets, heads)

Rows marked *optional* are shine (specular) maps: the symbol shows faintly
in the shine; replace them together with the picture they belong to.

| file (your PNG's name) | key | original | sizes (whole scales) | alpha | in MP | kind | shows |
|---|---|---|---|---|---|---|---|
| `of_c_wrmcht2_body_c` | d3f42c269537bc8a | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Military police (Feldgendarmerie) uniform: breast eagle with a swastika, a sleeve eagle and a cuff title |
| `of_c_wrmcht2_emb_c` | e962e352d4e48663 | 256x256 DXT1 | 256x256 ... 2048x2048 | opaque | no | party | Military police gorget ("Feldjaegerkorps"): an eagle over a wreath with a swastika |
| `c_wrmcht2_emb_s` | 75397864ba5e6644 | 128x128 DXT1 | 128x128 ... 1024x1024 | opaque | no | party | The gorget's shine (specular) map: the eagle and wreath show in the shine; optional, replace together with the gorget |
| `of_c_wrmcht_body_c` | 962d029613386d33 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Wehrmacht soldier's uniform: breast eagle with a swastika |
| `of_c_wrmcht_m_body_c` | 633eaf1badbc25d7 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Wehrmacht soldier's uniform (second version): breast eagle with a swastika |
| `c_wrmcht_m_body_s` | 4520f1fd14026faa | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | party | That uniform's shine (specular) map: the breast eagle shows in the shine; optional |
| `of_c_pnzgren_m_body1_c` | 4e388a3591c52f3a | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Panzergrenadier camouflage uniform (second version): an SS runes collar tab and an eagle belt buckle |
| `of_c_pnzcrew_body_c` | 87b8945b389f99e2 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Tank crew uniform: breast eagle with a swastika |
| `c_pnzcrew_body_s` | 28772a0c1a951e2b | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | party | The tank crew uniform's shine (specular) map: the breast eagle shows in the shine; optional |
| `of_c_fallsh_body1_c` | d992419503553590 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Paratrooper smock: the Luftwaffe breast eagle (its swastika isn't legible at this size) |
| `c_fallsh_body1_s` | e8263d9e2827f0f3 | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | party | The smock's shine (specular) map: the breast eagle shows in the shine; optional |
| `of_c_fallsh_i_body1_c` | eeeddb29a4538866 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | party | Paratrooper smock (second version): the Luftwaffe breast eagle (swastika not legible) |
| `c_fallsh_i_body1_s` | 1306e3940bd6d96f | 512x512 DXT1 | 512x512 ... 4096x4096 | opaque | no | party | That smock's shine (specular) map: the breast eagle shows in the shine; optional |
| `of_c_pnzgren_m_helm3_c` | e475257d3c826451 | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | Panzergrenadier helmet, plain (second version): SS runes and swastika shield decals |
| `of_c_pnzcrew_helm1_c` | b6a3260e9f47a7a8 | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | Tank crew helmet: an eagle decal with a swastika and a black-white-red shield decal |
| `of_c_pnzcrew_helm2_c` | 2765fcd1b83fdc1b | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | Helmet (tank crew and military police): an eagle decal with a swastika and a shield decal |
| `of_c_wrmcht_helm1_c` | c1ced8cb557f9ef9 | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | Wehrmacht soldier's helmet: an eagle decal with a swastika and a shield decal |
| `of_c_wrmcht_helm2_c` | a1a4bbf2b6acbcf0 | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | The same helmet with a net: eagle and shield decals |
| `of_c_wrmcht2_helm2_c` | 9afcca82b4f39ccf | 256x512 DXT1 | 256x512 ... 2048x4096 | opaque | no | party | Military police helmet with a net: eagle and shield decals |
| `of_c_wrmcht_head_c` | 16cc853f3766068a | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Wehrmacht soldier's head: the helmet's eagle decal with a swastika |
| `of_c_wrmcht2_head_c` | 9b7dccb60e954083 | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Military police head: the helmet's eagle decal with a swastika |
| `of_c_wrmcht2_heado_c` | 2cc18271227be39a | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Officer's head: a peaked cap with a cockade and an eagle |
| `of_c_pnzcrew_head1_c` | 21de7b985433e256 | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Tank crew head: a cap with a cockade in an oak wreath and an eagle |
| `of_c_pnzcrew_head2_c` | 8eab83b7a3079dd0 | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Tank crew head (second face): the same cap insignia |
| `of_c_pnzcrew_head3_c` | 9491be3c0b2c8ffb | 1024x512 DXT1 | 1024x512 ... 8192x4096 | opaque | no | party | Tank crew head (third face): the same cap insignia |

### The other levels: vehicles

Balkenkreuz crosses only (no swastika on any vehicle picture). The 512x256
pictures are smaller copies that the vehicles' simpler materials use. Where
two pictures have the same name, the second is saved as `<name>__<key>`.

| file (your PNG's name) | key | original | sizes (whole scales) | alpha | in MP | kind | shows |
|---|---|---|---|---|---|---|---|
| `of_king_tiger_c` | 468999288dcdec50 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | King Tiger tank: Balkenkreuz crosses |
| `king_tiger_d1_c` | 1a0aec7e363ff005 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The King Tiger, damaged: Balkenkreuz crosses |
| `king_tiger_d2_c` | 182096f365e9fa20 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The King Tiger wreck: Balkenkreuz crosses |
| `of_king_tiger` | d57a5d491442ec74 | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small King Tiger picture: Balkenkreuz crosses |
| `king_tiger_d1` | c3b4d90bbc23263f | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small damaged King Tiger picture: Balkenkreuz crosses |
| `of_tigeri_c` | 0a2b7714c6464a2e | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | transparent parts | no | military | Tiger I tank: Balkenkreuz crosses |
| `pantherv_c` | 56a4f6fe1b031b90 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Panther tank: Balkenkreuz crosses |
| `pantherv_d1_c` | 608483fdf90d9ec6 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The Panther, damaged: Balkenkreuz crosses |
| `panzezriv_c__ad6b7df34fd90727` | ad6b7df34fd90727 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Panzer IV tank (the second picture named panzezriv_c): Balkenkreuz crosses |
| `hanomag_c` | 387c71dfff04fc08 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Hanomag half-track: Balkenkreuz crosses (and a yellow division sign) |
| `hanomag_d1_c` | 1b8cea1fd9ac9b01 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The Hanomag, damaged: Balkenkreuz crosses |
| `hanomag` | c4908a0accdcfd5c | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small Hanomag picture: Balkenkreuz crosses |
| `sdk_c` | 18f5b36afa44be94 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Armored car (sdk): Balkenkreuz crosses |
| `sdk_d1_c` | cfd7b703cc7c0cb7 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The armored car, damaged: Balkenkreuz crosses |
| `v_sdk` | 5fe08560a857928a | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small armored car picture: Balkenkreuz crosses |
| `v_sdk_d1` | 0d1a0afae197aa04 | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small damaged armored car picture: Balkenkreuz crosses |
| `horch1a_c` | b18e34c32f0bc6cb | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | transparent parts | no | military | Horch staff car: Balkenkreuz crosses |
| `horch1a_d1_c` | a670e3c652a921aa | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | transparent parts | no | military | The Horch, damaged: a Balkenkreuz |
| `horch1a` | 9f752b9fe517a1b4 | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small Horch picture: Balkenkreuz crosses |
| `kubelwagon_c` | ef77aaf3d7262c24 | 1024x1024 DXT5 | 1024x1024 ... 8192x8192 | transparent parts | no | military | Kubelwagen: Balkenkreuz crosses |
| `kubelwagon_c__cfb80c49a03bd7a1` | cfb80c49a03bd7a1 | 1024x1024 DXT5 | 1024x1024 ... 8192x8192 | transparent parts | no | military | Kubelwagen (the second picture named kubelwagon_c): Balkenkreuz crosses |
| `kubelwagon_d1_c` | cfb209277e05351f | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | transparent parts | no | military | The Kubelwagen, damaged: a Balkenkreuz |
| `v_opel_blitz_c` | 4984862371805215 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Opel Blitz truck: a Balkenkreuz on the cab door |
| `v_opel_blitz2_c` | ae65b9fef2c908a6 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Opel Blitz truck (parked version): a Balkenkreuz on the cab door |
| `v_opel_blitz2_c__8a0ce8bcb0f7d890` | 8a0ce8bcb0f7d890 | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | Opel Blitz truck (the second picture named v_opel_blitz2_c): a Balkenkreuz on the cab door |
| `v_opel_blitz_d1_c` | b394221ddbeff4ad | 1024x1024 DXT1 | 1024x1024 ... 8192x8192 | opaque | no | military | The Opel Blitz, damaged: a Balkenkreuz on the cab door |
| `v_opel_blitz` | d28fedcd71397081 | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small Opel Blitz picture: a Balkenkreuz on the cab door |
| `v_opel_blitz_d1` | f9ca0abc54fc33ff | 512x256 DXT1 | 512x256 ... 4096x2048 | opaque | no | military | Small damaged Opel Blitz picture: a Balkenkreuz |

The list with every column (and where each texture is in the archives) is
`COD_3_WAW_Setup-tool/lists/insignia_textures.txt`.

### Where they are ("memory locations")

A texture has no fixed address while the game runs: the game unpacks its
archives into memory it picks at run time, a different place each run and
level. What identifies a texture in memory is its **key**, the hash of its
pixel data, which the game computes when it first draws the texture. That is
how the replacement finds it, and the key is what `texture_replacement_log`
prints (with that run's address after `at`).

On disk, `rip cod3-locate <sp folder> <name or key>...` prints where each
copy is: the archive, the chunk and its offset in the file, and the offsets
of the texture's header and pixels inside the unpacked chunk. The names
aren't in the game's program or level code at all; only the Saint-Lô level
script names the Fw 190's model parts (it flies over in a scripted event).
