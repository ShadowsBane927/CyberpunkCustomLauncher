import os

MENU = "assets/Menu_specific/Menu specific"
SAVE = "assets/Save_specific/Save specific"
FONTS = "assets"

entries = []  # (id, type, relative_path)

def rc(rid, path, rtype="RCDATA"):
    entries.append((rid, rtype, path))

# App icon - must be the FIRST resource declared so Windows picks it as the
# exe's default icon (Explorer/taskbar use the lowest-ID icon that appears
# first in the resource script).
rc("IDI_APPICON", "assets/app_icon.ico", "ICON")

# Fonts
rc("SV2_FONT_BOMBERTV", f"{FONTS}/EmblemaOne-Regular.ttf")
rc("SV2_FONT_BROADWAY", f"{FONTS}/Limelight-Regular.ttf")
rc("SV2_FONT_ENGRAVERS", f"{FONTS}/Cinzel-VF.ttf")
rc("SV2_FONT_REDMENACE", f"{FONTS}/red-menace.otf")
rc("SV2_FONT_DIGITAL7", f"{FONTS}/DSEG7Classic-Regular.ttf")

# Save editor static images
rc("SV2_BACKGROUND", f"{SAVE}/Background.png")
rc("SV2_DAVIDSHEAD", f"{SAVE}/DavidsHead.png")
rc("SV2_DAVIDSJACKET", f"{SAVE}/DavidsJacket.png")
rc("SV2_SANDEVISTAN", f"{SAVE}/Sandevistan.png")
rc("SV2_SAVE_LOCATION", f"{SAVE}/Save location.png")
rc("SV2_SHOT", f"{SAVE}/Shot.png")
rc("SV2_OUTSIDESHAPES", f"{SAVE}/OutsideShapes.png")
rc("SV2_IMAGE", f"{SAVE}/Image.png")
rc("SV2_LOCATION_PIN_ICON", f"{SAVE}/Location pin icon.png")
rc("SV2_LEVEL_AND_STREET_CRED_TEXT", f"{SAVE}/Level and Street Cred Text.png")
rc("SV2_BACK_TO_LAUNCHER", f"{SAVE}/Back to launcher.png")

months = ["JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE","JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"]
for m in months:
    rc(f"SV2_{m}", f"{SAVE}/{m.capitalize()}.png")

glasses = [
    ("ENGINEERING_GLASS", "Engineering Glass"),
    ("COMBAT_HACKING_GLASS", "Combat Hacking Glass"),
    ("HACKING_GLASS", "Hacking Glass"),
    ("TECHNICAL_ABILITY_GLASS", "Technical Ability Glass"),
    ("KENJUTSU_GLASS", "Kenjutsu Glass"),
    ("DEMOLITION_GLASS", "Demolition Glass"),
    ("INTELLIGENCE_GLASS", "Intelligence Glass"),
    ("COOL_GLASS", "Cool Glass"),
    ("REFLEXES_GLASS", "Reflexes Glass"),
    ("STRENGTH_GLASS", "Strength Glass"),
    ("GUNSLINGER_GLASS", "Gunslinger Glass"),
    ("CRAFTING_GLASS", "Crafting Glass"),
    ("ESPIONAGE_GLASS", "Espionage Glass"),
    ("STEALTH_GLASS", "Stealth Glass"),
    ("CONSOLE_GLASS", "Console Glass"),
    ("LOAD_SAVE_GLASS", "Load Save Glass"),
    ("SAVE_CHANGES_GLASS", "Save Changes Glass"),
    ("EXTRA_GLASS", "Extra glass"),
]
for rid, fname in glasses:
    rc(f"SV2_{rid}", f"{SAVE}/{fname}.png")
    rc(f"SV2_{rid}_INV", f"{SAVE}/{fname}_inverted.png")

# Menu screen images
idp = [
    ("IDP_BACKGROUND_PNG", "Background.png"),
    ("IDP_BUTTON_1_PNG", "Button 1.png"),
    ("IDP_BUTTON_1_INVERTED_PNG", "Button 1_inverted.png"),
    ("IDP_BUTTON_2_PNG", "Button 2.png"),
    ("IDP_BUTTON_2_INVERTED_PNG", "Button 2_inverted.png"),
    ("IDP_BUTTON_3_PNG", "Button 3.png"),
    ("IDP_BUTTON_3_INVERTED_PNG", "Button 3_inverted.png"),
    ("IDP_BUTTON_4_PNG", "Button 4.png"),
    ("IDP_BUTTON_5_PNG", "Button 5.png"),
    ("IDP_BUTTON_6_PNG", "Button 6.png"),
    ("IDP_BUTTON_7_PNG", "Button 7.png"),
    ("IDP_BUTTON_8_PNG", "Button 8.png"),
    ("IDP_BUTTON_9_PNG", "Button 9.png"),
    ("IDP_BUTTON_9_INVERTED_PNG", "Button 9_inverted.png"),
    ("IDP_CHECK__BUTTON_8__PNG", "Check (button 8).png"),
    ("IDP_CHECKED__BUTTON_8__PNG", "Checked (button 8).png"),
    ("IDP_SETTINGS_PNG", "Settings.png"),
    ("IDP_CHECK__BUTTON_4__PNG", "Check (button 4).png"),
    ("IDP_CHECK__BUTTON_5__PNG", "Check (button 5).png"),
    ("IDP_CHECKED__BUTTON_4__PNG", "Checked (button 4).png"),
    ("IDP_CHECKED__BUTTON_5__PNG", "Checked (button 5).png"),
    ("IDP_CITY_PNG", "City.png"),
    ("IDP_DAVID_MEMOIR_PNG", "David Memoir.png"),
    ("IDP_DAVID_GIF", "David.gif"),
    ("IDP_IMAGE_1_PNG", "Image 1.png"),
    ("IDP_IMAGE_2_PNG", "Image 2.png"),
    ("IDP_IMAGE_2_INVERTED_PNG", "Image 2_inverted.png"),
    ("IDP_IMAGE_3_PNG", "Image 3.png"),
    ("IDP_INVERTED_TO_NORMAL_TITLE_GIF", "Inverted to Normal Title.gif"),
    ("IDP_MOON_PNG", "Moon.png"),
    ("IDP_NORMAL_TO_INVERTED_TITLE_GIF", "Normal to Inverted Title.gif"),
    ("IDP_TITLE_PNG", "Title.png"),
    ("IDP_TITLE_INVERTED_PNG", "Title_inverted.png"),
]
for rid, fname in idp:
    rc(rid, f"{MENU}/{fname}")

# Verify every referenced file actually exists before writing the .rc
missing = []
for rid, rtype, path in entries:
    if not os.path.isfile(path):
        missing.append((rid, path))

if missing:
    print("MISSING FILES:")
    for rid, path in missing:
        print(f"  {rid} -> {path}")
else:
    print(f"All {len(entries)} resource files found OK.")

with open("resource.rc", "w", encoding="utf-8") as f:
    for rid, rtype, path in entries:
        # escape backslashes/quotes for RC string literal; use forward slashes (windres accepts them)
        esc = path.replace('\\', '/').replace('"', '\\"')
        f.write(f'{rid} {rtype} "{esc}"\n')

print(f"Wrote resource.rc with {len(entries)} entries")
