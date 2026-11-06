# Lumora: plan rozwoju

Stan na 2026-11-06 (wersja 0.1.0-beta.3). Plan obejmuje wszystko, czego silnikowi brakuje do
robienia gier, poza nowymi platformami natywnymi (macOS, Android, konsole). Web wchodzi.

Każdy punkt ma: **co**, **gdzie** (pliki), **jak** (podejście, co wykorzystać ponownie),
**sprawdzenie** i **rozmiar** (S: dni, M: tydzień, L: kilka tygodni, XL: miesiąc+).
Każdy punkt to osobny commit (lub kilka) z testami; wydania po każdej fazie.

---

## Faza 0: fundamenty (blokują resztę)

### 0.1 Compute i zapisywalne obrazy w render graphie (M): zrobione
Dziś nie ma ani jednego przebiegu compute ani obrazu do zapisu z shadera. Bez tego nie powstaną
TAA, VSM na GPU, mgła wolumetryczna, GI, cząsteczki na GPU ani streaming tekstur.
- **Gdzie:** `engine/renderer/RenderGraph.{hpp,cpp}`, `Gpu.hpp` (PipelineDesc, ShaderLibrary),
  `cmake/LumoraShaders.cmake` (etap `comp`), `TransientImagePool.cpp` (flaga UAV, mipy).
- **Jak:** `addComputePass`, `ImageUsage::Storage`, bufory w grafie (`createBuffer`, `readBuffer`
  z odczytem na CPU po N klatkach), `createComputePipeline` w `RenderContext`.
- **Sprawdzenie:** testy grafu (bariery, kolejność), test GPU: compute wypełnia obraz, render go czyta.

### 0.2 Prędkości i poprzednie macierze (S): zrobione
- **Gdzie:** `GpuTypes.hpp` (`FrameConstants::prevViewProjection`, `jitter`; `GpuObject::prevWorld`),
  `SceneRenderer.cpp` (pamięć poprzednich macierzy per obiekt, po kluczu encji jak w VSM),
  `depth.hlsl` (drugi cel: RG16F velocity).
- **Sprawdzenie:** test: obiekt przesunięty o 1 m daje oczekiwaną prędkość w pikselach.

### 0.3 Odpięcie Vulkana od wspólnego kodu (M)
Przygotowanie pod D3D12 i WebGPU.
- **Gdzie:** `Renderer.cpp` (`PresentTarget` z `VkSurfaceKHR`), `SceneRenderer::build`
  (`VkExtent2D`), `rhi::VulkanError` w kodzie wspólnym, nagłówki Vulkana w `TransientImagePool`,
  `FrameResources`, `GpuProfiler`, `EnvironmentCache`, `TextureCache.hpp`; `ShaderLibrary` (tylko `.spv`).
- **Jak:** własne typy `rhi::Extent`, `rhi::Format`, `rhi::GpuError`; Vulkan tylko w `renderer/vulkan/`.

### 0.4 Poprawka uploadu formatów blokowych (S)
`RenderContext::createTexture` liczy pitch jako `w * bytesPerBlock` (źle dla BC/ASTC).
Warunek dla kompresji tekstur (1.7).

---

## Faza 1: obraz

### 1.1 TAA (M): zrobione (bez upscalingu)
- **Jak:** jitter projekcji (Halton 2,3), historia jako obraz trwały (jak pula VSM), odrzucanie
  przez przycięcie do sąsiedztwa w YCoCg, wyostrzenie; przebieg między cząsteczkami a DoF.
  FXAA zostaje jako opcja „szybka”. Później: upscaling (render 67%, TAAU).
- **Skutek:** koniec migotania liści, cienkich krawędzi i ikon; gładkie wycięcia alfy.
- **Sprawdzenie:** render statycznej sceny w N klatkach zbiega się do obrazu supersamplingu;
  test duchów: obiekt w ruchu nie zostawia smugi > 2 px.

### 1.2 Prawdziwa przezroczystość (M): zrobione (bez załamania, które przechodzi do 2.3)
- **Stan:** `MeshRenderer.opacity` < 1 albo glTF BLEND z częściową alfą (współczynnik < 1 lub mapa,
  w której ponad 30% widocznych tekseli jest pośrednich) daje przebieg „Transparent”; BLEND z alfą
  0/1 (karty liści) zostaje wycięty. Bez cieni rzucanych i bez AO.
- **Gdzie:** nowy przebieg „Transparent” po `Water`, przed `Particles`; `BlendMode::Alpha`
  (już jest), `readDepth` jak cząsteczki; sortowanie od tyłu po środku obiektu.
- **Jak:** Mesh Renderer: tryb Opaque / Cut-out / Transparent; glTF BLEND przestaje być wycinany.
  Oświetlenie i cienie jak w Opaque (ten sam `mesh.hlsl` z wariantem). Szkło z załamaniem:
  kopia sceny przed przebiegiem i próbkowanie z przesunięciem (KHR_materials_transmission).
- **Sprawdzenie:** test GPU: półprzezroczysta płyta przed czerwonym tłem daje mieszany kolor;
  dwie płyty w złej kolejności w scenie dają poprawny wynik po sortowaniu.

### 1.3 Odbicia: SSR i sondy (L), zależy od 0.1
- **SSR:** hi-Z z głębi (compute), marsz promienia w przestrzeni ekranu, mieszanie z IBL po szorstkości.
- **Sondy odbić:** komponent `ReflectionProbe` (pudełko wpływu, paralaksa), przechwycenie
  6 widoków sceny w edytorze, prefiltracja istniejącym `EnvironmentMap.cpp::prefilterEnvironment`,
  zapis do `Saved/`; shader wybiera najbliższą sondę zamiast jednego globalnego nieba.
- **Woda:** dziś odbija tylko gradient nieba, dostanie SSR i sondy.

### 1.4 Globalne oświetlenie: wolumen sond (L), zależy od 1.3
- **Jak:** komponent `LightProbeVolume` (siatka), wypalanie w edytorze: dla każdej sondy render
  sześcianu niskiej rozdzielczości i rzut na SH9 (kod SH już jest w `EnvironmentMap.cpp`),
  zapis do tekstury 3D; shader interpoluje trójliniowo. Wypalanie wielokrotne daje odbicia światła.
  Lightmapy pominięte (wolumen sond pasuje do świateł dynamicznych i obiektów w ruchu).
- **Sprawdzenie:** czerwona ściana farbuje białą podłogę obok; test liczb SH na prostej scenie.

### 1.5 Mgła wolumetryczna i chmury (L), zależy od 0.1
- **Mgła:** froxele (np. 160×90×64), compute: rozpraszanie słońca z cieniem VSM i świateł lokalnych,
  całkowanie wzdłuż promienia; zastępuje dzisiejszą analityczną mgłę `fogParams` (z opcją zachowania).
  Promienie słońca (`SunShafts`) wynikną same.
- **Chmury:** warstwa raymarchingu w `sky.hlsl` z szumem 3D, ustawienia w środowisku sceny.

### 1.6 VSM sterowany przez GPU i cienie świateł lokalnych (L), zależy od 0.1
- **Jak:** compute oznacza strony z bufora głębi (tylko widoczne powierzchnie), odczyt na CPU z
  opóźnieniem 1-2 klatek, reszta jak dziś w `VirtualShadowMap.cpp` (pula, LRU, unieważnianie).
  Światła punktowe i reflektory: osobne wirtualne mapy zamiast atlasu 16 kafli.

### 1.7 Kompresja i streaming tekstur (L), zależy od 0.4
- **Kompresja:** BC7 (kolor), BC5 (normalne), BC4 (maski) kodowane przy imporcie do pamięci
  podręcznej w `Saved/`, plus wczytywanie KTX2/Basis (2.1). Na webie: transkodowanie Basis.
- **Streaming:** mipy dociągane według odległości i rozmiaru na ekranie, budżet VRAM, wyrzucanie
  nieużywanych (`TextureCache.cpp` dziś trzyma wszystko do końca).

### 1.8 Backend D3D12 (L), zależy od 0.3
- **Jak:** `NVRHI_WITH_DX12 ON`, kompilacja DXC do DXIL obok SPIR-V (atrybuty `[[vk::...]]` za makrami),
  wybór backendu w `RenderContext`, wybór w ustawieniach (Vulkan zostaje domyślny).
- **Sprawdzenie:** zestaw testów `RendererTest` na obu backendach w CI (Windows).

### 1.9 Pozostałe ulepszenia obrazu (M)
Rozmycie ruchu (z prędkości), automatyczna ekspozycja (histogram, compute), lepszy bloom
(fizyczny), cienie kontaktowe w przestrzeni ekranu, SSAO zastąpione GTAO.

---

## Faza 2: zasoby i materiały

### 2.1 Formaty obrazów: KTX2/Basis, WebP, EXR (M)
- **Gdzie:** `engine/assets/Image.cpp` (`decodeImage`), `GltfImporter.cpp` (`imageIndexOf`
  dziś pomija KTX2/WebP z ostrzeżeniem), `KHR_texture_basisu`, `EXT_texture_webp`.
- **Biblioteki:** libwebp (BSD), basis_universal transcoder (Apache-2.0), tinyexr (BSD).

### 2.2 Import OBJ i FBX (M)
- **Gdzie:** `engine/assets/AssetImport.cpp` (`isModelFile` dziś tylko .glb/.gltf), nowe
  `ObjImporter.cpp` i `FbxImporter.cpp` wypełniające ten sam `ImportedModel`.
- **Biblioteki:** tinyobjloader (MIT), ufbx (MIT, jeden plik; materiały, szkielety, animacje).
- **Sprawdzenie:** testy importu z małych plików w `tests/data`; jednostki FBX (cm) jak dla SketchUpa.

### 2.3 Rozszerzenia glTF (M)
`KHR_texture_transform`, `KHR_materials_clearcoat`, `sheen`, `specular`, `ior`, `transmission`
i `volume` (z 1.2), drugi zestaw UV, morph targets (dziś pomijane). Nowe pola w `GpuObject`
(bufor rozszerzeń materiałów zamiast wpychania w `surface`).

### 2.4 Materiały .lmat na równi z glTF (S)
`engine/assets/Material.cpp` nie ma okluzji, trybu alfy, dwustronności ani rozszerzeń; edytor
`MaterialEditor.cpp` dostaje te pola i podgląd na kuli.

### 2.5 Edytor materiałów w węzłach (L), zależy od 2.3
- **Jak:** front na istniejącym `GraphEditorPanel` (płótno, cofanie, schowek, komentarze już są);
  węzły: tekstury, UV i kafelkowanie, mieszanie warstw, maski, kolor, matematyka, czas, wiatr
  dla roślinności. Graf kompiluje się do fragmentu HLSL wstawianego w `mesh.hlsl` jako wariant;
  kompilator shaderów (shaderc/glslc) jedzie z edytorem, gra dostaje gotowe `.spv`.
- **Sprawdzenie:** graf „tekstura × kolor” daje ten sam obraz co .lmat z tymi ustawieniami.

---

## Faza 3: gameplay i narzędzia

### 3.1 UI gry (L)
Jest: `runtime/GameUi` (przycisk, pole wyboru, suwak, panel, etykieta, nawigacja padem),
`runtime/GameMenu`. Brakuje:
- **Obrazki** w HUD: `HudCanvas::images` z indeksem tekstury bindless; potok HUD ma już tryb
  kolorowego obrazka (bit1 w `hud.hlsl`, używany przez ikony edytora).
- **Układ:** zakotwiczenia, stosy i siatki, przewijanie; widżety zachowywane (nie tylko immediate).
- **Czcionki:** SDF zamiast jednego atlasu 48 px, więcej zakresów (cyrylica, greka, CJK opcjonalnie),
  własne czcionki z projektu.
- **Edytor UI:** płótno w edytorze z przeciąganiem widżetów, zapis `.lui`.
- **Wiązania:** brakuje Lua dla `audioBus`, węzłów wizualnych dla suwaka, pola wyboru,
  parametrów animatora i szyn audio.

### 3.2 Sieć w grach (XL)
- **Jak:** moduł `lumora_net` w `runtime/` bez Qt: UDP z kanałami niezawodnym i zawodnym,
  klient-serwer (host w grze), replikacja komponentów (Transform, fizyka, własne pola skryptów)
  z interpolacją, RPC ze skryptów (Lua, węzły, C++). Szyfrowanie: monocypher (już w projekcie),
  ramkowanie z `application/collab/Protocol.cpp` przeniesione na typy std.
  Adresy jak w collab (w tym tunele playit).
- **Sprawdzenie:** test dwóch symulacji w jednym procesie przez loopback: ruch hosta widać u klienta.

### 3.3 Debugger skryptów (L)
- **Lua:** hooki linii (`lua_sethook` z `LUA_MASKLINE`; dziś tylko licznik watchdoga), pułapki
  w marginesie `CodeEditorPanel`, krokowanie, zmienne lokalne i obserwowane (`lua_getlocal`).
- **Grafy:** pułapki na węzłach, pauza i krok (rozszerzenie `visual::GraphTrace`).
- **Pauza:** symulacja chodzi na wątku GUI (`EditorWindow.cpp`), więc pauza na pułapce przez
  zagnieżdżoną pętlę zdarzeń albo symulację na osobnym wątku.
- **C++:** przycisk „podepnij debugger” (gdb/Visual Studio) do procesu gry.

### 3.4 Animacja (L)
Drzewa mieszania 1D/2D, warstwy i maski kości, IK (dwie kości, patrzenie), root motion.
Gdzie: `runtime/AnimationSystem`, `AnimatorStateMachine` (format `.lanim`).

### 3.5 Audio (M)
Strefy pogłosu (węzły miniaudio), tłumienie przez przeszkody (raycast fizyki, filtr dolnoprzepustowy),
efekt Dopplera, strumieniowanie muzyki. Gdzie: `runtime/AudioWorld.cpp`.

### 3.6 Edytor terenu i roślinności (L)
Rzeźbienie, wygładzanie, malowanie warstw (splat), malowanie roślinności pędzlem.
Dziś teren to plik wysokości lub generator, roślinność tylko losowo (`Landscape.cpp`).

### 3.7 Cząsteczki na GPU (M), zależy od 0.1
Symulacja compute (dziś CPU w `engine/scene/Particles.cpp`), kolizje z głębią, sortowanie na GPU.

---

## Faza 4: web

Największe przedsięwzięcie: dzisiejszy gracz to edytor uruchomiony z `--play` (Qt, sam Vulkan,
wątki, `dlopen` modułu C++).

### 4.1 Gracz bez Qt (L), przyda się też na desktopie
Nowy `lumora-player` (SDL3 albo emscripten HTML5): okno, wejście, pad, pętla. Eksport desktopowy
też przechodzi na niego (mniejsze paczki, bez Qt w grach).

### 4.2 Backend WebGPU (XL), zależy od 0.3
NVRHI nie ma WebGPU, więc własny cienki backend na Dawn/emdawnwebgpu dla podzbioru potrzebnego
grze. Shadery: HLSL → SPIR-V (już) → WGSL przez Tint. Na start bez VSM (cienie kaskadowe),
bez bindless (tablice tekstur).

### 4.3 Środowisko web (M)
JobSystem i Jolt w trybie jednego wątku (lub pthreads z nagłówkami COOP/COEP), zasoby w paczce
w MEMFS, tylko Lua i grafy (bez modułów C++), audio przez Web Audio (miniaudio to wspiera).

### 4.4 Eksport „Web” i CI (M)
Platforma w `GameExporter` i oknie eksportu, Build Support „Web” w Hubie, zadanie emsdk w
`release.yml`, test uruchomienia w przeglądarce headless.

---

## Faza 5: jakość i ulepszenia przekrojowe

- **Wydajność:** profiler Tracy, wątek renderowania, culling na GPU, meshlety, budżety klatki.
- **Edytor:** wyszukiwanie zasobów, podgląd materiałów, prefaby zagnieżdżone, wieloscenowość.
- **Stabilność:** testy wizualne (porównanie obrazów z wzorcami), testy na obu platformach przy
  każdym PR, zrzuty awarii.
- **Dokumentacja:** `docs/GUIDE.md` i `docs/SCRIPTING.md` aktualizowane z każdą funkcją;
  przewodnik migracji przy zmianach ABI skryptów (`LUMORA_SCRIPT_ABI`).

---

## Kolejność

1. **Faza 0** (fundamenty): bez niej nic dalej się nie zepnie.
2. **1.1 TAA, 1.2 przezroczystość, 3.1 UI gry**: najwięcej widać i najczęściej potrzeba w grach.
3. **2.1-2.3 import i materiały, 1.7 kompresja**: duże sceny i modele z sieci.
4. **1.3-1.6 odbicia, GI, mgła, VSM na GPU**: jakość obrazu.
5. **3.2-3.7 sieć, debugger, animacja, audio, teren, cząsteczki.**
6. **1.8 D3D12, 2.5 materiały w węzłach.**
7. **Faza 4 web** (po 0.3 i 4.1).

Niezależne tory, które mogą iść równolegle (różne pliki): renderer (faza 0-1), zasoby
(2.1-2.2), UI gry (3.1), sieć (3.2), debugger (3.3).

## Sprawdzanie każdego punktu

- testy jednostkowe w `tests/` (CPU) i `RendererTest` z `Renderer::capture` (GPU, piksele);
- render bez okna (`Lumora --headless --render`) z bliska i z daleka, obejrzany przed commitem;
- `ctest` na Linuxie lokalnie, a w CI na Linuxie i Windowsie (oba wymagane do wydania);
- dokumentacja zaktualizowana w tym samym commicie.
