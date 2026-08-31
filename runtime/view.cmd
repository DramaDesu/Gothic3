@echo off
rem Opens the viewer without anyone having to type four archive paths.
rem
rem   view              one sector: the fortress, with its objects, grass and trees
rem   view world        the whole map, which takes a couple of minutes to load
rem   view tree         a row of five red oaks grown from one definition
rem   view tree <name>  the same for another definition, e.g. varant/g3_tree_m_coconutpalm_01.spt
rem
rem Set G3 to your install if it is not where this expects it.

if "%G3%"=="" set "G3=F:\SteamLibrary\steamapps\common\Gothic 3"
if not exist "%G3%\Data\_compiledMesh.pak" (
    echo Cannot find the game at "%G3%".
    echo Set G3 to the folder holding Gothic3.exe and run this again.
    pause
    exit /b 1
)

set "HERE=%~dp0"
set "VIEWER=%HERE%build\g3world.exe"
if not exist "%VIEWER%" (
    echo The viewer is not built yet: %VIEWER%
    pause
    exit /b 1
)

set "MESHES=%G3%\Data\_compiledMesh.pak"
set "SECTORS=%G3%\Data\Projects_compiled.pak"
set "TREES=%G3%\Data\Speedtrees.pak"

rem The viewer looks for its shaders beside itself.
cd /d "%HERE%build"

if /i "%~1"=="world" goto :world
if /i "%~1"=="tree" goto :tree

echo One sector: 1344 objects, 2559 plants, 146 trees.
"%VIEWER%" "%MESHES%" none --sectors "%SECTORS%" x55000y0z55000_cstat.node --tree "%TREES%"
goto :done

:world
echo The whole map. This reads 2177 sectors and takes a couple of minutes.
"%VIEWER%" "%MESHES%" g3_world_lowpoly_landscape_01/ --sectors "%SECTORS%" _cstat.node --tree "%TREES%"
goto :done

:tree
set "WHICH=%~2"
if "%WHICH%"=="" set "WHICH=myrtana/g3_tree_m_redoak_01.spt"
echo Five of %WHICH%, grown from one definition with five seeds.
"%VIEWER%" "%MESHES%" none --tree "%TREES%" "%WHICH%"
goto :done

:done
if errorlevel 1 (
    echo.
    echo The viewer exited with an error. The lines above say why.
    pause
)
