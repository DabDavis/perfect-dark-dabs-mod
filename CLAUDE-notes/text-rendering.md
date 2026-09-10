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

The port draws the border itself when Clean Text Outlines is on (Dab's Mod
menu, `Mod.CleanTextOutlines`, default on): `SHADER_OPT_TEXT_OUTLINE` in
`gfx_opengl.cpp` takes the body alpha of tile 1 half a texel out in eight
directions and uses that, capped by the cell, as tile 0's alpha. It is set in
`gfx_derive_batch_state()` when tile 0 is an outline glyph
(`TEXPACK_GLYPH_IS_OUTLINE`) that no texture pack replaced - a pack's
`outlines/` image is what its author wanted. The per-combiner program table
has a second half for it (`prg[32]`, bit 4 of `tm`).
