# Text rendering

`textMeasure()` adds a line of height only when it sees `\n`. A string without a
trailing newline measures as zero height and renders clipped. ROM strings have it;
anything synthesised needs it too.

`text0f15568c()` drops any glyph whose x is past `viGetWidth()`, whatever the
scissor says. The menu is 320x220 units at every window size and is drawn
centred at the view's own aspect, so on a widescreen display there is a pillar
of screen either side that no dialog reaches — and a bigger coordinate does not
get you there, because the text stops at the edge of the view. What does is
`G_ASPECT_CENTER_EXT` being `LEFT | RIGHT`: dropping one half holds what
follows against that edge, at the size and shape it already had. Ghost Trials'
nameplate and rules windows do that. A scissor set before the alignment stays
behind in the middle of the screen, because fast3d turns a scissor into pixels
when the command is sent.

A dialog is as wide as its widest *row*, and a `MENUITEMTYPE_LABEL` does not
count towards that: a long label is drawn from the dialog's left edge and
simply runs off it, with no wrap and no ellipsis to say it did. The small font
gets about **46 characters** onto one line of a full width dialog before the
tail is lost. Ghost Trials' status lines are written to that budget - "Set a
Security Question to create an account, or Sign In." is 57 and lost its last
three words on screen while measuring fine in a header. Two shorter labels
stacked is the fix; the account pages use that.

## The dark box behind outlined text is the font, not the renderer

`textRender` (the outline pass) draws one glyph twice in a two-cycle
combiner: tile 0 through TLUT bank 0 of `var8007fb5c`, whose alpha covers
body and border, tile 1 through bank 1, the body alone. The border the font
bakes in is not a border: every texel of the cell that is not body is
outline index 7, opaque, with partial indices only at the corners. An 'e' is
a solid black 7x7 block with red strokes in it. At 320x240 that reads as a
bold outline; at 1080p, magnified 4.5x with bilinear filtering, it is a black
slab behind every letter, and a lighter backdrop shows it as "the transparent
glyph square". A whole session went looking for this in the palette
conversion, the two-cycle shader, the blend mode and the texture cache; the
glyph dump (`import_texture_ci4`, indices and alphas per row) settled it in
one look. Dump the texels before suspecting the renderer.

The port draws the border itself when Thin Text Outlines is on (Dab's Mod
menu, default on - the row was called Clean Text Outlines until 2026-09-12
and its key is still `Mod.CleanTextOutlines`, so that nobody's setting is
lost to the rename): `SHADER_OPT_TEXT_OUTLINE` in
`gfx_opengl.cpp` takes the body alpha of tile 1 half a texel out in eight
directions and uses that, capped by the cell, as tile 0's alpha. It is set in
`gfx_derive_batch_state()` when tile 0 is an outline glyph
(`TEXPACK_GLYPH_IS_OUTLINE`) that no texture pack replaced - a pack's
`outlines/` image is what its author wanted. The per-combiner program table
has a second half for it (`prg[32]`, bit 4 of `tm`).

Off is the ROM's filled cell, which is what that switch is for. It is not
what the **XBLA release's** font does: it serves tile 0's picture itself and
has a band for either position of the switch - thin when it is on, a bold
black border when it is off - because the ROM's cell drawn around another
font's letter is a block with somebody else's letter punched out of it. See
xbla.md, "The outline pass cannot be left to the shader".

## The menu's overlay pass sets the primitive colour once, for every item

`menuRenderDialog()` draws each dialog twice over its rows: the items, and
then an **overlay** pass for the few item types that have to draw over the
rows below them (a dropdown's open list, the player-stats panel). That second
pass sets the render state it wants **once, before the loop**:
`textSetPrimColour(gdl, 0x00000000)`, which is `G_CC_PRIMITIVE` with a
transparent primitive colour.

`menuitemListOverlay()` relies on that and nothing else. It draws a rectangle
over the whole of its list and carries no colour of its own, so it paints
nothing - as long as nothing earlier in the same pass has changed the
combiner or the primitive colour. An **open dropdown standing above a list in
the same dialog** does exactly that: the pass walks a column's rows in order,
the dropdown's overlay goes through `menuitemListRender()` and
`textRenderProjected()`, which sets its own combiner and a primitive colour
per glyph, and the list's rectangle below it is then filled with the colour of
the last option drawn. On Ghost Trials' Leaderboards page - Mission dropdown,
Difficulty dropdown, then the list of times - that is a solid pane of menu
teal over the times, brightening and dimming as the focused option pulses,
and it is the *list* that paints it, not the dropdown.

No stock dialog puts a dropdown above a list, which is why the ROM never shows
it. `menuitemListOverlay()` sets the state it needs itself now.

Worth knowing for anything else drawn in that pass: **the state a menu draw
inherits is whatever the previous item left**, and the pass makes no promise
beyond its first line. It also means a wrongly-coloured rectangle is not
necessarily drawn by the thing it appears on top of. What found this was
logging every `gfx_dp_fill_rectangle`, `gfx_dp_texture_rectangle` and
`gfx_sp_tri_emit` in `gfx_pc.cpp` whose screen box covered one pixel in the
middle of the blob, and diffing the list with the dropdown open against the
list with it closed; reading the menu code found four wrong answers first.
