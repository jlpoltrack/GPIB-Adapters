# Fit test

Region (board frame, mm): x -3.0..22.0, y 26.0..50.0. Print `fit-test-corner-bottom.3mf` and `fit-test-corner-top.3mf` as they are (already in print orientation, no supports); `fit-test-corner.png` shows the cut. The board overhangs the cut edges: judge only the features below.

Bring: the assembled board.

## Checks

### Seam and board seat
- [ ] board sits flat on the 1.0 mm ledge, 0.25 mm side clearance (no forcing, no rattle)
- [ ] top skirt slides over the 0.9 mm lip (0.15 mm slide gap): no binding, no visible gap at the seam
- [ ] halves sit flush all round, no step at the seam
- [ ] no warping: halves flat on the bed and on a table

### Snap (left, 31)
- [ ] snaps in with a clear click (0.35 mm ridge); closing force: light / firm / too hard
- [ ] opens again by hand without cracking the skirt; ridge not flattened or sheared

### Snap (back, 18)
- [ ] snaps in with a clear click (0.35 mm ridge); closing force: light / firm / too hard
- [ ] opens again by hand without cracking the skirt; ridge not flattened or sheared

### LED tube (2.2, 38.6)
- [ ] tube centred over the LED, 0.3 mm above it (not touching)
- [ ] LED clearly visible through the roof hole

### Flex button (6.0, 44.3)
- [ ] stem lands on the switch actuator (0.4 mm free play, not pre-pressed)
- [ ] press clicks the switch, tab springs back; force: light / firm / too hard
- [ ] tab free in its slot (no bridging strings), no crack at the hinge

### Post (18.5, 45.2)
- [ ] post lands on bare board (0.05 mm gap), no part under it
- [ ] board held down: no rattle with the lid closed

## Not covered (check on the full print)

- Connector window
- USB-C pocket
- RJ45 window
- Snap (left, 10)
- Snap (right, 7.5)
- Snap (back, 31)
- Screw boss (5.2, 10.7)
- Screw boss (52.0, 10.7)
- USB-C brace
- RJ45 brace
- Post (8.0, 20.0)

## Report back

Per check: OK, or what was wrong with a number (mm, which direction, how hard). Also: printer, filament, layer height, any first-layer squish or elephant foot.
