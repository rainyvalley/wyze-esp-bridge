// Enclosure for wyze-esp-bridge:
//   USB-C power socket -> antenna overhang -> ESP32-S3-DevKitC-1 (flat, no headers)
//   -> USB-C-to-A OTG adapter -> Wyze Sense Bridge dongle standing like a plate across the +X end.
//
// All dimensions in mm. Parts are raised/offset automatically so the board's OTG port,
// the adapter and the dongle's USB-A plug line up; the case grows to fit.
// Render: F6, export STL: F7. Print base upright, lid plate-down (as laid out with part = "both").

/* [Part] */
part = "both"; // [both, base, lid]

/* [ESP32-S3 board] */
board_l = 58.8;    // PCB length (X), without antenna overhang
board_w = 28.65;   // PCB width (Y)
pcb_t = 1.75;      // PCB thickness
pin_h = 0;         // height of anything under the PCB (no headers soldered = 0)
comp_h = 3.0;      // tallest component above the PCB
port_off = 1.48;   // USB-C port centre above the PCB top
port_ovh = 1.18;   // USB-C ports stick out this far past the PCB edge (+X)
ant_ovh = 5.99;    // antenna overhang past the PCB (-X)
otg_y = 21.65;     // OTG port centre from the PCB's -Y edge
com_y = 7.0;       // COM port centre from the PCB's -Y edge (assumed mirror of otg_y; measure)

/* [OTG adapter, USB-C plug -> USB-A socket] */
otg_body = 20.75;  // PCB edge -> USB-A face, adapter plugged in
otg_bw = 14.62;    // body width (Y)
otg_bh = 7.06;     // body height (Z)
otg_cz = 3.15;     // USB-C plug centre above the body bottom
otg_az = 4.17;     // USB-A socket centre above the body bottom
otg_cy = 7.31;     // USB-C plug centre from the body's -Y side
otg_ay = 7.31;     // USB-A socket centre from the body's -Y side

/* [Wyze Sense Bridge dongle: PLACEHOLDERS, measure (back face toward you, plug horizontal)] */
dongle_w = 30;     // width (Y)
dongle_h = 30;     // height (Z)
dongle_t = 10;     // thickness (X), back face to front face, excluding the plug
plug_y = 15;       // plug centre from the dongle's -Y edge
plug_z = 15;       // plug centre above the dongle's bottom edge

/* [USB-C power socket (molded pigtail receptacle)] */
pwr_w = 13.37;     // width (Y)
pwr_h = 5.26;      // height (Z)
pwr_d = 8.21;      // body depth, front face -> wire exit (X)
pwr_clr = 0.2;     // fit clearance around the socket (snug; add a drop of glue)
pwr_notch = 6;     // wire notch width in the rear stop

/* [Case] */
wall = 2;
floor_t = 2;
lid_t = 2;
clr = 0.4;         // general fit clearance, tune for your printer
lip_h = 3;         // lid locating lip depth
lip_t = 1.2;       // lid lip thickness
tab_gap = 0.2;     // gap between hold-down tabs and the part they hold
tab_w = 4;         // hold-down tab footprint
vents = true;      // lid vents over the antenna end
com_slot = false;  // side slot for a right-angle USB-C cable into the COM port
com_slot_l = 12;   // slot length (X)
com_slot_h = 8;    // slot height (Z)
show_parts = false; // preview (F5) only: ghost models of the board, adapter, dongle and socket

$fn = 32;
eps = 0.01;

// ------------------------------------------------------------------ layout
// Interior coordinates: x = 0 at the inner -X wall face, y = 0 at the inner -Y wall face, z = 0 on the floor.

port_cz = pcb_t + port_off;  // port centre above the board bottom

// Z: lift the board until the adapter and the dongle clear the floor
zb = max(pin_h, otg_cz - port_cz, plug_z - otg_az + otg_cz - port_cz);
adp_z0 = zb + port_cz - otg_cz;
a_cz = adp_z0 + otg_az;
dng_z0 = a_cz - plug_z;

// X
stop_t = 2;
stop_x = pwr_d - wall + pwr_clr;          // socket front face sits flush with the outer wall
bay_l = stop_x + stop_t + 1;
bx0 = bay_l + ant_ovh + clr;              // board -X edge
bx1 = bx0 + board_l;                      // board +X edge (port side)
adp_x0 = bx1 + port_ovh;                  // adapter body start (approx.)
a_face = bx1 + otg_body;                  // USB-A face
dng_x1 = a_face + dongle_t;
in_l = dng_x1 + clr;

// Y (board frame: board -Y edge = 0)
adp_y0f = otg_y - otg_cy;
a_cyf = adp_y0f + otg_ay;
dng_y0f = a_cyf - plug_y;
y_min = min(0, adp_y0f, dng_y0f);
y_max = max(board_w, adp_y0f + otg_bw, dng_y0f + dongle_w);
by0 = clr - y_min;                        // board -Y edge in interior coordinates
in_w = max(y_max - y_min + 2 * clr, pwr_w + 2 * pwr_clr + 4);
adp_y0 = by0 + adp_y0f;
dng_y0 = by0 + dng_y0f;
pwr_yc = in_w / 2;

in_h = max(zb + pcb_t + comp_h, adp_z0 + otg_bh, dng_z0 + dongle_h, pwr_h + pwr_clr) + clr;

out_l = in_l + 2 * wall;
out_w = in_w + 2 * wall;
out_h = in_h + floor_t;

echo(str("Outer size (base): ", out_l, " x ", out_w, " x ", out_h, " mm, lid adds ", lid_t));
echo(str("Board lifted ", zb, " mm; adapter bottom ", adp_z0, "; dongle bottom ", dng_z0));
echo("WARNING: dongle_* and plug_* are placeholders until the Sense Bridge is measured.");

// ------------------------------------------------------------------ base

module in_frame() { translate([wall, wall, floor_t]) children(); }

module base() {
    difference() {
        cube([out_l, out_w, out_h]);
        in_frame() translate([0, 0, 0]) cube([in_l, in_w, in_h + eps]);
        // Power socket opening in the -X wall
        in_frame() translate([-wall - eps, pwr_yc - pwr_w / 2 - pwr_clr, 0])
            cube([wall + 2 * eps, pwr_w + 2 * pwr_clr, pwr_h + 2 * pwr_clr]);
        // Optional COM port side slot (-Y wall)
        if (com_slot)
            in_frame() translate([bx1 + port_ovh, -wall - eps, zb + port_cz - com_slot_h / 2])
                cube([com_slot_l, wall + 2 * eps, com_slot_h]);
    }
    in_frame() {
        power_cradle();
        board_supports();
        adapter_cradle();
        dongle_support();
    }
}

module power_cradle() {
    y0 = pwr_yc - pwr_w / 2 - pwr_clr;
    y1 = pwr_yc + pwr_w / 2 + pwr_clr;
    rail_h = pwr_h * 0.8;
    // Side guides
    for (y = [y0 - 1.5, y1]) translate([0, y, 0]) cube([stop_x, 1.5, rail_h]);
    // Rear stop with a wire notch
    difference() {
        translate([stop_x, y0 - 1.5, 0]) cube([stop_t, y1 - y0 + 3, pwr_h + 1]);
        translate([stop_x - eps, pwr_yc - pwr_notch / 2, 1]) cube([stop_t + 2 * eps, pwr_notch, pwr_h + 1]);
    }
}

module board_supports() {
    if (zb > 0.2) {
        // Rails under both long edges, plus a cross rib at each end
        for (y = [by0, by0 + board_w - 2]) translate([bx0, y, 0]) cube([board_l, 2, zb]);
        for (x = [bx0, bx1 - 2]) translate([x, by0, 0]) cube([2, board_w, zb]);
    }
    // Side locators: keep the board from sliding in Y
    loc_h = zb + pcb_t;
    for (x = [bx0 + board_l * 0.3, bx0 + board_l * 0.7]) {
        translate([x, by0 - clr - 1.2, 0]) cube([3, 1.2, loc_h]);
        translate([x, by0 + board_w + clr, 0]) cube([3, 1.2, loc_h]);
    }
}

module adapter_cradle() {
    len = a_face - adp_x0 - 1;
    // Support under the adapter body
    if (adp_z0 > 0.2) translate([adp_x0 + 0.5, adp_y0 + 1, 0]) cube([len, otg_bw - 2, adp_z0]);
    // Side walls to half the body height
    side_h = adp_z0 + otg_bh / 2;
    for (y = [adp_y0 - clr - 1.2, adp_y0 + otg_bw + clr])
        translate([adp_x0 + 0.5, y, 0]) cube([len, 1.2, side_h]);
}

module dongle_support() {
    if (dng_z0 > 0.2) translate([a_face + 0.5, dng_y0 + 1, 0]) cube([dongle_t - 1, dongle_w - 2, dng_z0]);
    // Low side guides
    for (y = [dng_y0 - clr - 1.2, dng_y0 + dongle_w + clr])
        translate([a_face, y, 0]) cube([dongle_t, 1.2, dng_z0 + 3]);
}

// ------------------------------------------------------------------ lid

// A hold-down tab hanging from the lid down to top_z (interior z) + tab_gap.
module tab(x, y, top_z, w = tab_w, d = tab_w) {
    len = in_h - top_z - tab_gap;
    if (len > 0) translate([x - w / 2, y - d / 2, top_z + tab_gap]) cube([w, d, len + eps]);
}

module lid() {
    difference() {
        union() {
            translate([0, 0, out_h]) cube([out_l, out_w, lid_t]);
            // Locating lip inside the walls
            in_frame() difference() {
                translate([clr / 2, clr / 2, in_h - lip_h]) cube([in_l - clr, in_w - clr, lip_h + eps]);
                translate([clr / 2 + lip_t, clr / 2 + lip_t, in_h - lip_h - eps])
                    cube([in_l - clr - 2 * lip_t, in_w - clr - 2 * lip_t, lip_h + 3 * eps]);
            }
        }
        if (vents) {
            vx0 = bay_l + 1;
            vx1 = bx0 + 15;
            for (y = [wall + 4 : 3.2 : out_w - wall - 5])
                translate([wall + vx0, y, out_h - eps]) cube([vx1 - vx0, 1.6, lid_t + 2 * eps]);
        }
    }
    in_frame() {
        // Board: press on the PCB edge strips
        for (x = [bx0 + board_l * 0.5, bx1 - 6])
            for (y = [by0 + 1.5, by0 + board_w - 1.5])
                tab(x, y, zb + pcb_t, w = 3, d = 2);
        tab((adp_x0 + a_face) / 2, adp_y0 + otg_bw / 2, adp_z0 + otg_bh);
        tab(a_face + dongle_t / 2, dng_y0 + dongle_w / 2, dng_z0 + dongle_h);
        tab(stop_x / 2, pwr_yc, pwr_h);
    }
}

// ------------------------------------------------------------------ fit check

module ghosts() {
    in_frame() {
        // Board + antenna overhang + components + ports
        color("green") translate([bx0, by0, zb]) cube([board_l, board_w, pcb_t]);
        color("green") translate([bx0 - ant_ovh, by0 + board_w / 2 - 9, zb]) cube([ant_ovh, 18, pcb_t]);
        color("silver") translate([bx0 + 2, by0 + 5, zb + pcb_t]) cube([25, 18, comp_h]);
        for (y = [otg_y, com_y]) color("silver")
            translate([bx1 - 6, by0 + y - 4.5, zb + port_cz - 1.6]) cube([6 + port_ovh, 9, 3.2]);
        color("gray") translate([adp_x0, adp_y0, adp_z0]) cube([a_face - adp_x0, otg_bw, otg_bh]);
        color("white") translate([a_face, dng_y0, dng_z0]) cube([dongle_t, dongle_w, dongle_h]);
        color("black") translate([-wall, pwr_yc - pwr_w / 2, 0]) cube([pwr_d, pwr_w, pwr_h]);
    }
}

// ------------------------------------------------------------------ output

if (show_parts) %ghosts();

if (part == "base" || part == "both") base();
if (part == "lid") translate([0, out_w, out_h + lid_t]) rotate([180, 0, 0]) lid();
if (part == "both") translate([0, 2 * out_w + 5, out_h + lid_t]) rotate([180, 0, 0]) lid();
