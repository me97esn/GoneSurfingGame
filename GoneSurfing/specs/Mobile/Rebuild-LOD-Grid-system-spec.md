Spec: optimize wave lod

## use case:
I have a simulation of an ocean with a breaking wave, made in blender with the flip fluids plugin. I want to use this animation for visuals in a mobile game, deployed to android

I export the visuals only of this simulation to Unreal Engine, package and install on an android phone. I don't use collision or physics or anything apart from the looks of the wave. 

## Current solution
Since mobile devices don't support Alembic animations, I export the visuals as static meshes, and change mesh each tick to achieve a stop motion effect. 

The entire animation is apx 200 frames, and loops infinitely. 

To achieve high fps I have a custom made lod system, which splits the meshes into a grid of 8*3 chunks. I have one set of high resolution chunks, and one set of low resolution chunks. If the camera is nearby, the high res mesh is loaded and displayed, otherwise the low res is used. I don't use culling yet. 
I have adjusted the distance for high resolution meshes so that at any given time, 4 chunks are high resolution, while 20 chunks are low res. 

The low res meshes are not changed every tick. They have lower framerate, and currently only update every 2:nd tick.

The script that exports the ocean wave as mesh chunks is E:\windowsgrejor\git\GoneSurfingSimulation\scripts\export_waves_display_parallel.sh

The Unreal code that handles the streaming of mesh chunks are these two classes:
-GridLodActor
-InfiniteWaveManager

When the camera moves sideways, the GridLodActor furthest away is moved to a new position. This is done to create the illusion of an infinite wave. The actor is not instanciated but moved, to make sure any preloaded meshes are preserved. 

## What currently works well:
I mostly have high fps, around 50.
The visuals are really good. 
The wave flows naturally most of the time. 
## Current problems:
Sometimes the animation freezes, or grinds to real slow. When this happens, the game is non responsive. 
The game takes a long time to start, often over a minute, with just a black screen while I'm waiting for it to load. 
The game is way too big. It requires 25GB on the android phone. 
Things to consider when rebuilding this:
Unreal has built in lod and hlod. Would that work better for this use case? 
The size of the assets for low res is only slightly lower then the assets for high res. Even though the number of vertices is 80-90% lower. The low res meshes (obj files) are apx 4 kB each, while the important unreal asset for the same mesh is 54KB big. 
The high res obj files on the other hand are 10 kB while the unreal asset for the high real mesh is 62 kB. These are stored in folders:
D:\gone_surfing_exports\medium_wave_left\chunks_ratio_0_005
D:\gone_surfing_exports\medium_wave_left\chunks_ratio_0_03
E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Content\Waves\chunks_ratio_0_03
E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\Content\Waves\chunks_ratio_0_005
Note how little difference there is between asset size even though the mesh size is less then half.
I have read that Unreal handles fewer bigger meshes better than many small. Is that correct in this use case? 
Changing both the export script, the code in GridLodActor and GridLodManager, and the engine code of Unreal engine is ok
Quicker initial load is a nice to have. Perhaps a simple solution would be to load the meshes in the background, while displaying a progress bar or similar. The time is not the important part, but feedback is
If one mesh isn't used because the frame rate is this resolution is too low to display it, deleting the mesh asset world make the app smaller 
There is no reason why I have a custom lod system other then to optimize the size and performance of a stop motion 3d animation. It can be rewritten from scratch if there are better ways to do it. 
Look at the older specs LOD_Grid_System_Spec.md and InfiniteWaveManager.spec.md for understanding current solution. But don’t be restricted to these two specs.

## acceptance criteria 
- 3d Animation of the wave flows at 50fps all of the time on a mobile device
- The size of the game is no more then 1GB
- The camera can still move sideways an infinite distance, and the GridLodActors follow so that there is always a wave in view. 
## Nice to haves
- The game takes at most 20 seconds to load
- A progress bar is displayed when the game starts, until enough meshes has been preloaded to run animation smoothly
- The export script takes a very long time to complete. Around 15 hours per compression ratio. If this could be done faster would be nice.
- The import of the meshes into Unreal Engine takes a long time to complete. Usually around 8 hours per compression ratio. This would also be nice if it could be done faster.
