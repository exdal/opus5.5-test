-- oxcity override: fetch-only recipe. the upstream recipe builds libXcursor from x.org tarballs, which the
-- sandbox proxy blocks. tools/sandbox/bootstrap.sh extracts Ubuntu's libxcursor-dev into .sandbox/sysroot
-- instead, and this recipe hands xmake those headers and libs
package("libxcursor")
    set_homepage("https://www.x.org/")
    set_description("X11 libXcursor (from the oxcity sandbox sysroot)")

    on_fetch("linux", function (package, opt)
        local sandbox = os.getenv("OXCITY_SANDBOX")
        if not sandbox then
            return
        end
        local sysroot = path.join(sandbox, "sysroot")
        local libdir = path.join(sysroot, "usr", "lib", "x86_64-linux-gnu")
        if os.isfile(path.join(libdir, "libXcursor.so")) then
            return {includedirs = {path.join(sysroot, "usr", "include")}, linkdirs = {libdir}, links = {"Xcursor"}}
        end
    end)
