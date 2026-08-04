# F4SEVRSerializationFix
## Upgrade for F4SEVR's serialization to handle light mods and saves imported from flat Fallout4.

With the addition of [Daytripper4](https://www.nexusmods.com/fallout4/mods/91141), Fallout 4 VR can now deal correctly with 'light' plugins, bringing the VR experience much closer to the 'flat' Fallout4.

#### But not F4SEVR.  
Based on an old version of F4SE, it uses an obsolete serialization format, and ignores any light plugins.
This F4SEVR plugin patches F4SEVR at runtime to give it serialization compatible with modern versions of F4SE.

Do you need this plugin?
Yes, if:
- You want to load and exchange save files between flat Fallout and VR. 
- If you have any mods with light plugins that use F4SE serialization for things like: Key, controller, external event registrations or other mod-specific data. 

Important: if you install this mod, you cannot uninstall it, as without it, F4SEVR will no longer be able to read the saved information.

Credits, to the usual heroes of the modding community:

- [Ian Patterson](https://github.com/ianpatt) and the whole F4SE crew.
- [AlanTse](https://github.com/alandtse), for the indispensable, VR-Compatible CommonLibF4 .


