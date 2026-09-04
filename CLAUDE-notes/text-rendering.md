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
