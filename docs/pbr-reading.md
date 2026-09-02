# Physically based rendering: what to read

Collected for the eventual PBR layer over this runtime. The order matters more
than the list: each of these assumes the one before it.

Read the measurement first, because it decides what any of this can mean here.
Gothic 3's 1150 materials carry **no metallic and no roughness texture at all** -
0 of 1150 bind anything to SpecularPower. What exists is a diffuse map, an
optional DXT5nm normal map, a greyscale specular *mask* and one shininess scalar
per material, which the game's own shipped shader turns into a Phong exponent by
multiplying by 128. So a physically based layer here is **authoring**, not
restoration: roughness has to be derived from the specular mask, and metallic
invented outright. Worth doing as an improvement; not worth calling faithful.

## The ground

**Naty Hoffman, "Background: Physics and Math of Shading"** - the SIGGRAPH course
chapter that explains where the BRDF comes from, why energy conservation matters
and what a microfacet model actually claims. Everything below is an engineering
answer to questions this poses. In the 2013 and 2016 course archives below.

**Physically Based Rendering: From Theory to Implementation**, Pharr, Jakob and
Humphreys - the reference work, and the third and fourth editions are both free
in full at [pbr-book.org](https://pbr-book.org/). It is an offline renderer, not
a real-time one, which is exactly why it is worth having: the real-time papers
are all approximations of what this book does exactly, and knowing the exact
version makes the approximations legible. Read the reflection models chapter
rather than the whole thing.

## What real engines actually shipped

**Brian Karis, "Real Shading in Unreal Engine 4"** (SIGGRAPH 2013) - the canonical
talk, and the one to read first for practice: the split-sum approximation for
image-based lighting, the specular BRDF choices with their reasons, and the
mobile compromises.
[Notes (PDF)](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf)
· [Slides (PDF)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_slides.pdf)

**Lagarde and de Rousiers, "Moving Frostbite to Physically Based Rendering"**
(SIGGRAPH 2014) - the most useful of the lot for us, because it is about
*converting an existing engine* rather than building one: what breaks, what the
old content does to the new model, how legacy materials are migrated. That is our
situation exactly.
[Presentation](https://www.slideshare.net/slideshow/moving-frostbite-to-physically-based-rendering/41070721)

**Brent Burley, "Physically-Based Shading at Disney"** (SIGGRAPH 2012) - the
Disney BRDF, which both of the above build on. Short, and the source of the
parameter set everyone now uses.

**SIGGRAPH 2016 course, "Physically Based Shading in Theory and Practice"** -
[the whole archive](https://blog.selfshadow.com/publications/s2016-shading-course/),
and the 2012 to 2020 courses are all on that site. The single best place to
follow how the field settled.

## The one to implement from

**Filament's documentation**, Google -
[Physically Based Rendering in Filament](https://google.github.io/filament/Filament.md.html).
A complete, current, real-time model written down in one document, with the maths
and the reasoning for every choice rather than only the result. If we write a PBR
path, this is the one to follow, because it is a specification rather than a
talk - and Filament is Vulkan-first, like us.

## What it would cost here, concretely

- **Roughness** must be derived. The specular masks are 239 images, 228 of them
  DXT1 with no alpha, 201 greyscale within 2%, median value 0.069 - a mask, not a
  gloss map. Mapping it to roughness is a decision, not a conversion.
- **Metallic** does not exist. Everything is dielectric unless we author
  otherwise, which for armour and weapons is the whole point of doing it.
- **The lighting we have is baked**, and baked light times a PBR BRDF is not
  physically anything. Either the patches become an irradiance term used properly,
  or PBR waits for real-time lighting - which is where ray tracing comes back
  into it.
- **The tone curve is the game's own**: (1 - exp(-2.85c))^0.60. A PBR pipeline
  wants its own exposure and tone mapping, and the two cannot both be in charge.
