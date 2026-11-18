"""What the FPS Analyzer costs inside a running OBS (obs-websocket).

    python obs-cost.py "<capture source name>" [--seconds 20] [--port 4455] [--password ...]

Measures OBS's own statistics (GetStats: CPU usage of the OBS process, average
frame render time, frames skipped by the renderer) for --seconds in each of:

  1. the FPS Analyzer filter on, overlay graphs as they are
  2. the filter off
  3. the filter on, all overlay graphs off
  4. the filter on, all overlay graphs on

and prints the difference to (2). Every setting it touches is put back at the
end, also on Ctrl+C. It refuses to run while recording or streaming, since
switching the filter changes the output.

fps-bench measures the algorithm offline; this shows the real cost in OBS,
frame copies and overlay rendering included.
"""
import argparse
import sys
import time

import obsws_python as obs

GRAPH_KEYS = ("show_pacing_graph", "show_frametime_graph", "show_fps_graph")


def measure(cl, seconds):
    a = cl.get_stats()
    cpu, rt, n = 0.0, 0.0, 0
    t_end = time.time() + seconds
    while time.time() < t_end:
        time.sleep(1.0)
        s = cl.get_stats()
        cpu += s.cpu_usage
        rt += s.average_frame_render_time
        n += 1
    b = cl.get_stats()
    total = b.render_total_frames - a.render_total_frames
    skipped = b.render_skipped_frames - a.render_skipped_frames
    return {"cpu": cpu / max(n, 1), "render_ms": rt / max(n, 1), "skipped": skipped, "frames": total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=4455)
    ap.add_argument("--password", default="")
    args = ap.parse_args()

    cl = obs.ReqClient(host=args.host, port=args.port, password=args.password, timeout=10)
    if cl.get_record_status().output_active or cl.get_stream_status().output_active:
        sys.exit("recording or streaming is active: switching the filter would change the output")

    filt = next((f for f in cl.get_source_filter_list(args.source).filters
                 if f["filterKind"] == "fps_analyzer_filter"), None)
    if not filt:
        sys.exit("no FPS Analyzer filter on '%s'" % args.source)
    scene = cl.get_current_program_scene().current_program_scene_name
    overlays = [i["sourceName"] for i in cl.get_scene_item_list(scene).scene_items
                if i["inputKind"] == "fps_overlay_source"]
    saved = {o: {k: cl.get_input_settings(o).input_settings.get(k) for k in GRAPH_KEYS} for o in overlays}
    was_enabled = filt["filterEnabled"]

    def graphs(on):
        for o in overlays:
            cl.set_input_settings(o, {k: on for k in GRAPH_KEYS}, True)

    rows = []
    try:
        cl.set_source_filter_enabled(args.source, filt["filterName"], True)
        rows.append(("filter on, graphs as set", measure(cl, args.seconds)))
        cl.set_source_filter_enabled(args.source, filt["filterName"], False)
        rows.append(("filter off", measure(cl, args.seconds)))
        cl.set_source_filter_enabled(args.source, filt["filterName"], True)
        if overlays:
            graphs(False)
            rows.append(("filter on, all graphs off", measure(cl, args.seconds)))
            graphs(True)
            rows.append(("filter on, all graphs on", measure(cl, args.seconds)))
    finally:
        cl.set_source_filter_enabled(args.source, filt["filterName"], was_enabled)
        for o, vals in saved.items():
            cl.set_input_settings(o, {k: v for k, v in vals.items() if v is not None}, True)

    off = rows[1][1]
    print("%-28s %9s %10s %12s %10s" % ("", "OBS CPU", "render ms", "vs filter off", "skipped"))
    for name, r in rows:
        print("%-28s %8.2f%% %10.3f %+11.3f ms %5d/%d" % (name, r["cpu"], r["render_ms"],
                                                        r["render_ms"] - off["render_ms"],
                                                        r["skipped"], r["frames"]))


if __name__ == "__main__":
    main()
