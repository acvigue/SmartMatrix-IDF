# Render duration 13300

load("render.star", "render")
load("encoding/base64.star", "base64")

WIFI_ICON = base64.decode("""iVBORw0KGgoAAAANSUhEUgAAABcAAAAMCAYAAACJOyb4AAAAAXNSR0IArs4c6QAAAG5JREFUOE9jZKAhYKSh2QyEDd/+/j9OB3gK4tWP33CQwfgMICCP23BCBsO8g0cddsPRNWALGmQf4bCAuDDHFjRE+Iz4MEd2PcwyssMcOYlgCyYCKQWknXCw4LOEQCYh3nBswUI1w0EGERGJyPYBAOlZNg3qWQzkAAAAAElFTkSuQmCC""")
def main():
    return render.Root(
        child = render.Stack(
            children = [

                # column to hold stuff at the top of the screen
                render.Column(
                    children = [

                        # row to hold text with equal space between them
                    render.Box(
                    width = 64,
                    height = 11,
                    padding = 0,
                    color = "#001253",
                    child = render.Text("Setup Wifi", color = "#DDD", font = "Dina_r400-6", offset = 0),
                            )],
                        
                    
                ),

                #column to hold the stuff at the bottom of the screen
                render.Column(
                    main_align = "end",  # bottom
                    expanded = True,
                    children = [

                        # row to hold text evenly distrubuted accross the row
render.Marquee(
     width=64,
     child=render.Text("Download ESP SoftAP Prov from the App Store", color = "#f2c665", font = "tb-8", offset = 1),
     offset_start=64,
     offset_end=0,
)
                    ],
                ),

                # column to hold the stuff in the middle (center) of the screen
                render.Column(
                    main_align = "center",
                    expanded = True,
                    children = [
                        # row to hold text with equal space around each item
                        render.Row(
                            main_align = "space_evenly",
							cross_align="center",
                            expanded = True,
                            children = [
                            
								render.Image(src=WIFI_ICON)
                             
                            ],
                        ),
                    ],
                ),
            ],
        ),
    )
