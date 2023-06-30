# Render duration 8300

load("render.star", "render")
load("encoding/base64.star", "base64")

WIFI_ICON = base64.decode("""
R0lGODlhFwAMAPAAAAAAAPXknCH/C05FVFNDQVBFMi4wAwEAAAAh/wtJbWFnZU1hZ2ljaw1nYW1tYT0wLjQ1NDU1ACH5BAEUAAAALAAAAAAXAAwAAAIRhI+py+0Po5y02otZyKHzVwAAIf8LSW1hZ2VNYWdpY2sNZ2FtbWE9MC40NTQ1NQAh+QQBFAAAACwAAAAAFwAMAAACFISPqcvtD6OctNqALQiHa/9l2vYUACH/C0ltYWdlTWFnaWNrDWdhbW1hPTAuNDU0NTUAIfkEARQAAAAsAAAAABcADAAAAhiEj6nL7Q+jnChYS0FIW3aehcoXXqJBLgUAIf8LSW1hZ2VNYWdpY2sNZ2FtbWE9MC40NTQ1NQAh+QQBFAAAACwAAAAAFwAMAAACGYSPqcvdAWNwaSpL8c2c+g+G4qGN0YiUSgEAIf8LSW1hZ2VNYWdpY2sNZ2FtbWE9MC40NTQ1NQAh+QQBFAAAACwAAAAAFwAMAAACEYSPqcvtD6OctNqLWcih81cAACH/C0ltYWdlTWFnaWNrDWdhbW1hPTAuNDU0NTUAIfkEARQAAAAsAAAAABcADAAAAhSEj6nL7Q+jnLTagC0Ih2v/Zdr2FAAh/wtJbWFnZU1hZ2ljaw1nYW1tYT0wLjQ1NDU1ACH5BAEUAAAALAAAAAAXAAwAAAIYhI+py+0Po5woWEtBSFt2noXKF16iQS4FACH/C0ltYWdlTWFnaWNrDWdhbW1hPTAuNDU0NTUAIfkEARQAAAAsAAAAABcADAAAAhmEj6nL3QFjcGkqS/HNnPoPhuKhjdGIlEoBADs=
""")
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
                    child = render.Text("Check Wifi", color = "#DDD", font = "Dina_r400-6", offset = 0),
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
					child=render.Text("Checking Wifi Settings", color = "#f2c665", font = "tb-8", offset = 1),
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
