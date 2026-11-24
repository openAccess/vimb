-- Archiving signed in: sign in to a site in the sign-ins (served by
-- signin-server.py on 127.0.0.1:9446), then archive it signed in, and not.
-- The assistant's and the container's tests serve on 9446 too: stop the
-- server once this test is done.
--   vimb -C "set archive-allow-internal=true" --script tests/archive/signin-test.lua
local site = os.getenv("VIMB_SIGNIN_SITE") or "http://127.0.0.1:9446"
local failures = 0
local function check(what, ok, detail)
    vimb.log((ok and "ok   " or "FAIL ") .. what .. (detail and (": " .. tostring(detail)) or ""))
    if not ok then failures = failures + 1 end
end

-- nothing to start signed in with yet
local none, err = vimb.archive.start{name = "too soon", signed_in = true}
check("no sign-ins, no signed-in archive", none == nil and tostring(err):find("no sign%-ins"), err)

-- signing in, in the sign-ins
local s, info = vimb.signin(site .. "/login")
check("signed in, in the sign-ins", info and info.title == "Signed in", info and info.title)
s:close()

-- the person's own browsing isn't signed in
local plain = vimb.open(site .. "/private")
check("browsing isn't signed in", plain.title == "Sign in", plain.title)
plain:close()

-- an archive started signed in is; one started otherwise isn't
local a = assert(vimb.archive.start{name = "signed", signed_in = true})
local t, i = a:open(site .. "/private")
check("an archive started signed in is signed in", i and i.title == "Members", i and i.title)
t:load(site .. "/rotate")
t:close()
vimb.log("SIGNED " .. assert(a:stop()))
local b = assert(vimb.archive.start{name = "unsigned"})
t, i = b:open(site .. "/private")
check("one started otherwise isn't", i and i.title == "Sign in", i and i.title)
t:close()
vimb.log("UNSIGNED " .. assert(b:stop()))

-- clearing the sign-ins
check("cleared", vimb.ex("archive signins clear"))
none, err = vimb.archive.start{name = "after clearing", signed_in = true}
check("none after clearing", none == nil, err)

vimb.log(failures == 0 and "all ok" or (failures .. " failed"))
if failures > 0 then error(failures .. " checks failed") end
