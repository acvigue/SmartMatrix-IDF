# Exmaple of how to use Stack with Rows and Columns to control quadrant position of text on the tidbyt.
# Render duration: 19350

load("render.star", "render")
load("encoding/base64.star", "base64")

WIFI_ICON = base64.decode("""
iVBORw0KGgoAAAANSUhEUgAAABcAAAAMCAYAAACJOyb4AAAAAXNSR0IArs4c6QAAAIpJREFUOE+1kjEOgCAMRekVdNVN738g3XTVK5R0+AkS2080MJGQ/vdaKqnjkY7ZyQ1XVRURuaZFPYHx3EO58BEAL9zAEcANR+EfwGt4bWSAe14fDZTGXgf0Q70O2EjMJAwvA+w+HFuyDmDNANTcDMoQjIhtCjWvt6T1k1HXZA57FJk126LP5oAwQAbBb2kNuzsgSAAAAABJRU5ErkJggg==
""")
def main():
    return render.Root(
        child = render.Stack(
            children = [

                # column to hold stuff at the top of the screen
                render.Column(			
                    children = [
					
					
                    render.Box(
                    width = 64,
                    height = 11,
                    padding = 0,
                    color = "#CF0A0A",
                    child = render.Text("Oops!", color = "#DDD", font = "Dina_r400-6", offset = 0),
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
					child=render.Text("Provisioning Failed...", color = "#f2c665", font = "tb-8", offset = 1),
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
