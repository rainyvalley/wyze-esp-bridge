// Enclosure for wyze-esp-bridge on the Waveshare ESP32-P4-WIFI6-POE-ETH.
//
// The board sits on four standoffs. The lid has matching spacer tubes, so four M2.5 screws go
// lid -> tube -> PCB -> standoff and hold both the board and the lid.
// Front wall: stacked USB-A (dongle) / USB-C (power, console), RJ45 (PoE), TF card.
// Left wall: RESET and BOOT button holes (press with a pin). Lid: PWR LED hole, vents over the PoE supply.
//
// Board frame: x = 0 at the left edge (buttons side), y = 0 at the connector edge, z = 0 at the PCB top.
// Board positions come from Waveshare's dimension drawing and PCB layout. Values marked MEASURE are
// estimates: check them with calipers before printing.
// Render: F6, export STL: F7. Print the base upright and the lid upside down (as laid out with part = "both").

/* [Part] */
part = "both"; // [both, base, lid]
show_board = false; // preview (F5) only: ghost model of the board and connectors

/* [Board] */
board_w = 55;      // X
board_d = 68;      // Y
pcb_t = 1.6;
hole_x = [3.5, 51.5];    // 48 mm apart
hole_y = [25.0, 58.0];   // 33 mm apart, from the connector edge
hole_d = 2.7;            // M2.5
below_clear = 4;         // tallest part under the PCB (bridge rectifiers, diodes)          MEASURE
top_clear = 14.5;        // tallest part above the PCB top (RJ45, PoE caps/transformer)       MEASURE
conn_ovh = 1.75;         // connectors stick out past the PCB edge

/* [Front connectors (x = centre from the left edge; z relative to the PCB top)] */
usb_x = 12.2;
usba_w = 13.6;     // USB-A shell width
usba_z0 = 0.0;     // USB-A shell bottom                                                    MEASURE
usba_h = 7.0;      // USB-A shell height                                                    MEASURE
usbc_zc = -3.2;    // USB-C centre (the stack's USB-C sits below the PCB)                   MEASURE
usbc_open_w = 12.5; // opening for a USB-C plug's overmold
usbc_open_h = 7.0;
rj_x = 29.95;
rj_w = 16.1;
rj_z0 = 0.0;       // RJ45 body bottom (negative if it sinks through the PCB)               MEASURE
rj_h = 13.5;       //                                                                       MEASURE
tf_open = true;    // TF card slot opening
tf_x = 45.2;
tf_w = 12.5;
tf_z0 = 0.0;
tf_h = 2.2;

/* [Left-edge buttons and LED] */
btn_y = [12.5, 5.8]; // RESET, BOOT (from the connector edge)
btn_z = 1.6;         // actuator centre above the PCB top                                   MEASURE
btn_d = 3.5;
led_xy = [1.2, 18.4]; // PWR LED (lid light hole)
led_d = 2.5;

/* [Case] */
wall = 2;
floor_t = 2;
lid_t = 2;
clr = 0.5;         // gap between the PCB and the walls
conn_clr = 0.5;    // extra opening size around connectors (each side)
lip_h = 3;          // lid lip on the left, right and back walls (the front wall has the connectors)
lip_t = 1.2;
lip_clr = 0.3;
roof_extra = 2;      // extra height above top_clear: keeps the wall above the RJ45 opening thick
snap = true;         // groove inside the base walls + ridge on the lid lip: the lid clicks on without screws
snap_depth = 0.5;    // groove depth into the wall
snap_h = 1.4;        // groove height
snap_grip = 0.3;     // how far the lid ridge reaches into the groove (more = tighter, tune for your printer)
standoff_d = 6;
pilot_d = 2.2;     // 2.2 for M2.5 self-tapping into plastic, 3.6 for M2.5 heat-set inserts
tube_d = 5.4;      // lid spacer tubes
screw_clear_d = 2.9;
head_d = 5.2;      // screw head counterbore in the lid
head_h = 1.2;
vents = true;
ears = false;      // screw-down tabs on the left and right
ear_hole_d = 4.2;

$fn = 40;
eps = 0.01;

// ------------------------------------------------------------------ layout

standoff_h = below_clear + 1;
in_w = board_w + 2 * clr;
in_d = board_d + conn_ovh + clr;
in_h = standoff_h + pcb_t + top_clear + roof_extra;
out_w = in_w + 2 * wall;
out_d = in_d + 2 * wall;
out_h = floor_t + in_h;

// Board frame -> case coordinates
bx = wall + clr;
by = wall + conn_ovh;                 // connector faces touch the front wall's inner face
bz = floor_t + standoff_h + pcb_t;    // PCB top

screw_len = lid_t + top_clear + roof_extra + pcb_t + standoff_h + floor_t - 0.6 - 0.5;   // longest that fits
echo(str("Outer size: ", out_w, " x ", out_d, " x ", out_h + lid_t, " mm (with lid)"));
echo(str("Screws: 4x M2.5, ", floor(screw_len) - 2, "-", floor(screw_len), " mm long"));
echo(str("Wall above the RJ45 opening: ", out_h - (bz + rj_z0 + rj_h + conn_clr), " mm"));
echo("Check every value marked MEASURE with calipers before printing.");

// ------------------------------------------------------------------ helpers

// Opening through the front wall, centred on board x, spanning board z0..z0+h (both plus clearance).
module front_cut(x, w, z0, h) {
    translate([bx + x - w / 2 - conn_clr, -eps, bz + z0 - conn_clr])
        cube([w + 2 * conn_clr, wall + 2 * eps, h + 2 * conn_clr]);
}

// Triangle-profile strip along X (length len), h tall at y = 0, tapering to a line at y = depth.
module ridge(len, depth, h) {
    hull() {
        translate([0, -0.3, 0]) cube([len, 0.3 + eps, h]);   // 0.3 mm root inside the parent solid
        translate([0, depth - eps, h / 2 - eps / 2]) cube([len, eps, eps]);
    }
}

snap_z = out_h - lip_h / 2 - snap_h / 2;   // groove / ridge bottom

module slot(len, w) { hull() for (i = [0, 1]) translate([i * (len - w), 0, 0]) cylinder(d = w, h = lid_t + 2 * eps); }

// ------------------------------------------------------------------ base

module base() {
    difference() {
        union() {
            cube([out_w, out_d, out_h]);
            if (ears)
                for (side = [0, 1])
                    translate([side ? out_w : -12, out_d / 2 - 8, 0])
                        difference() {
                            cube([12, 16, 3]);
                            translate([side ? 7 : 5, 8, -eps]) cylinder(d = ear_hole_d, h = 3 + 2 * eps);
                        }
        }
        translate([wall, wall, floor_t]) cube([in_w, in_d, in_h + eps]);
        front_cut(usb_x, usba_w, usba_z0, usba_h);
        front_cut(usb_x, usbc_open_w - 2 * conn_clr, usbc_zc - usbc_open_h / 2 + conn_clr, usbc_open_h - 2 * conn_clr);
        front_cut(rj_x, rj_w, rj_z0, rj_h);
        if (tf_open) front_cut(tf_x, tf_w, tf_z0, tf_h);
        for (y = btn_y)
            translate([-eps, by + y, bz + btn_z]) rotate([0, 90, 0]) cylinder(d = btn_d, h = wall + 2 * eps);
        if (snap) {
            // left and right inner walls (strip along Y, cutting outward)
            translate([wall + eps, wall, snap_z]) rotate([0, 0, 90]) ridge(in_d, snap_depth + eps, snap_h);
            translate([wall + in_w - eps, wall + in_d, snap_z]) rotate([0, 0, -90]) ridge(in_d, snap_depth + eps, snap_h);
            // back inner wall
            translate([wall, wall + in_d - eps, snap_z]) ridge(in_w, snap_depth + eps, snap_h);
        }
    }
    // Standoffs
    for (x = hole_x, y = hole_y)
        translate([bx + x, by + y, floor_t - eps])
            difference() {
                cylinder(d = standoff_d, h = standoff_h + eps);
                translate([0, 0, 0.6 - floor_t]) cylinder(d = pilot_d, h = standoff_h + floor_t);   // pilot runs into the floor
            }
}

// ------------------------------------------------------------------ lid (modelled in place, flipped for printing)

module lid() {
    tube_len = out_h - bz - 0.2;   // down to the PCB top, less 0.2 mm so the screws clamp the PCB
    difference() {
        union() {
            translate([0, 0, out_h]) cube([out_w, out_d, lid_t]);
            // Locating lip on the left, right and back (none at the front: the connectors reach the top there)
            lx0 = wall + lip_clr;
            ly0 = wall + lip_clr + 6;           // stops short of the connectors
            lx1 = wall + in_w - lip_clr;
            ly1 = wall + in_d - lip_clr;
            translate([0, 0, out_h - lip_h]) {
                translate([lx0, ly0, 0]) cube([lip_t, ly1 - ly0, lip_h + eps]);
                translate([lx1 - lip_t, ly0, 0]) cube([lip_t, ly1 - ly0, lip_h + eps]);
                translate([lx0, ly1 - lip_t, 0]) cube([lx1 - lx0, lip_t, lip_h + eps]);
            }
            // Snap ridges on the lip's outer faces, matching the base grooves
            if (snap) {
                rd = lip_clr + snap_grip;
                rh = snap_h - 0.4;
                rz = snap_z + 0.2;
                translate([lx0 + eps, ly0 + 2, rz]) rotate([0, 0, 90]) ridge(ly1 - ly0 - 4, rd, rh);
                translate([lx1 - eps, ly1 - 2, rz]) rotate([0, 0, -90]) ridge(ly1 - ly0 - 4, rd, rh);
                translate([lx0 + 2, ly1 - eps, rz]) ridge(lx1 - lx0 - 4, rd, rh);
            }
            // Spacer tubes down to the PCB
            for (x = hole_x, y = hole_y)
                translate([bx + x, by + y, out_h - tube_len]) cylinder(d = tube_d, h = tube_len + eps);
        }
        // Screw holes with counterbores
        for (x = hole_x, y = hole_y) translate([bx + x, by + y, 0]) {
            translate([0, 0, bz - eps]) cylinder(d = screw_clear_d, h = out_h - bz + lid_t + 2 * eps);
            translate([0, 0, out_h + lid_t - head_h]) cylinder(d = head_d, h = head_h + eps);
        }
        // PWR LED light hole
        translate([bx + led_xy[0], by + led_xy[1], out_h - eps]) cylinder(d = led_d, h = lid_t + 2 * eps);
        // Vents over the PoE transformer and capacitors
        if (vents)
            for (y = [26 : 4 : 44])
                translate([bx + 31, by + y, out_h - eps]) slot(17, 2);
    }
}

// ------------------------------------------------------------------ fit check

module board_ghost() {
    translate([bx, by, bz]) {
        color("green") translate([0, 0, -pcb_t]) cube([board_w, board_d, pcb_t]);
        color("silver") translate([usb_x - usba_w / 2, -conn_ovh, usba_z0]) cube([usba_w, 17.9, usba_h]);
        color("silver") translate([usb_x - 4.5, -conn_ovh, usbc_zc - 1.6]) cube([9, 7, 3.2]);
        color("silver") translate([rj_x - rj_w / 2, -conn_ovh, rj_z0]) cube([rj_w, 21.5, rj_h]);
        color("black") translate([tf_x - 6, 0, 0]) cube([12, 12.6, 1.8]);
        color("black") translate([2, board_d - 5.5, 0]) cube([51, 5, 8.5]);            // 2x20 header
        color("gray") translate([33.7, 29, 0]) cube([13, 14, 10]);                      // PoE transformer
        for (p = [[50.8, 32], [50.8, 42]]) color("blue") translate([p[0], p[1], 0]) cylinder(d = 7, h = 10.5);
        color("blue") translate([43, 20.4, 0]) cylinder(d = 8, h = 12.5);               // 22 uF 100 V
    }
}

// ------------------------------------------------------------------ output

if (show_board) %board_ghost();
if (part == "base" || part == "both") base();
if (part == "lid") translate([0, out_d, out_h + lid_t]) rotate([180, 0, 0]) lid();
if (part == "both") translate([out_w + 20, out_d, out_h + lid_t]) rotate([180, 0, 0]) lid();
