aelkey = require("aelkey")

aelkey.monitor.watch("switch_watch", {
  {
    id = "switch_pro",
    type = "evdev",
    vid_pid = {
      {0x057e, 0x2009}, -- Nintendo Switch Pro Controller
    }
  },
})

function on_watch(events)
  print("Watchlist on_watch:")
  print(aelkey.util.dump_table(events))
end

print("Watchlist entries:")
for _, ref in ipairs(aelkey.monitor.watchlist()) do
  print("  -", ref)
end

aelkey.monitor.set_callback("on_watch")

aelkey.start()
