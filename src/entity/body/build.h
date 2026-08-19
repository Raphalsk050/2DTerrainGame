#pragma once

// What a body is made of, as numbers.
//
// Everything here used to be `player_config` — a namespace of constants, which
// is exactly right while there is one body in the world and exactly wrong the
// moment there are two. A constant cannot differ between a player and a pig, so
// the alternative to this struct is a second copy of the whole walk: gravity,
// the ledge it steps over, the corner it is nudged past, the water it floats
// in. That copy is where a mob starts walking through hillsides the player is
// stopped by, and nothing about the two files says they were ever meant to
// agree.
//
// So the walk is code and the body is data, which is the same split the rest of
// this project already makes between `Chunk` and `kElements`. A new creature is
// a row of these numbers; the code that carries it over a ledge is written once.
//
// Every figure is in world pixels, or pixels per second, or pixels per second
// squared. Read them against the character they were tuned for: it is 26 tall,
// 12 wide, and jumps 72.
namespace body {

struct Build {
    // The box the world is asked about. The crouched body is shorter and just as
    // wide.
    float width  = 12.0f;
    float height = 26.0f;

    // A body that never crouches leaves this at its full height, which makes
    // the stance a no-op rather than a special case anybody has to test for.
    float crouchHeight = 26.0f;

    float runSpeed    = 220.0f;
    float crouchSpeed = 90.0f;

    // Held speed. Equal to the walk for anything that has only one gait, so
    // asking a body to hurry is always a legal question with a dull answer.
    float sprintSpeed = 220.0f;

    float groundAccel = 2200.0f;
    float airAccel    = 1200.0f; // Reduced control while airborne.

    float gravity      = 1600.0f;
    float maxFallSpeed = 900.0f;

    // Upward launch speed. Peak height is jumpSpeed^2 / (2 * gravity).
    float jumpSpeed = 480.0f;

    // Releasing the jump mid-rise clamps the remaining upward speed to this,
    // giving a short hop for a tap and a full arc for a hold. Clamping rather
    // than scaling keeps the result the same however many frames the button
    // stays up.
    float jumpCutSpeed = 190.0f;

    // Height of a ledge the body walks over instead of being stopped by.
    //
    // The ground is sampled on a six pixel lattice and terraced on top of that,
    // so small steps are everywhere — a hillside is a staircase and a cave floor
    // is gravel. Without this a body is stopped dead by every one of them.
    float stepHeight = 14.0f;

    // How far a body is pulled back down onto the ground after it leaves it
    // while still walking, so running downhill is a run and not a skip.
    float snapDistance = 14.0f;

    // How far sideways a body may be nudged to clear a corner its head caught
    // on. Under half the width, so it can never be moved somewhere it could not
    // already stand.
    float cornerNudge = 5.0f;

    // How far a body is carried to get out of ground that closed around it.
    //
    // Past this the honest answer is that it is buried, and lifting one out of
    // solid rock across half a screen would be a worse surprise than the burial.
    float unstickReach = 52.0f;

    // A jump still fires this long after walking off a ledge, and a jump pressed
    // this long before landing is remembered and fires on contact.
    float coyoteTime = 0.10f;
    float jumpBuffer = 0.12f;

    // Upward push from liquid, as a multiple of gravity at full submersion.
    // Above 1 so a body that sinks in comes back up and settles at the surface.
    float buoyancy = 1.4f;

    // Rate at which liquid bleeds off velocity, per second at full submersion.
    float waterDrag = 5.0f;

    // Submersion at which control switches from walking to swimming.
    float swimThreshold = 0.25f;

    float swimSpeed  = 150.0f;
    float swimStroke = 200.0f;

    // Downward stroke while holding crouch under water. Without it a body can
    // only bob at the surface, since buoyancy always wins once submerged.
    float sinkStroke   = 170.0f;
    float waterMaxFall = 200.0f;

    // A body gravity does not act on, which steers in both axes and still
    // collides with everything. A bat, not a ghost — see Body::Ghost for the
    // other one, which is a debug flight and passes through rock.
    //
    // The two are deliberately different fields in different places: one is a
    // fact about the creature and belongs on its row, the other is a state the
    // player toggles and belongs on the instance.
    bool floats = false;

    // Steering speed and acceleration while floating. Ignored otherwise.
    float floatSpeed = 160.0f;
    float floatAccel = 900.0f;

    // Free flight, for looking at the world rather than playing in it.
    //
    // Its own three numbers rather than the walk's, and that is not tidiness: it
    // is faster than running and much faster still while boosted, because what it
    // is for is crossing enough of the world to judge how it was generated. The
    // acceleration is high enough that it stops where the key is released, since
    // drifting past what is being looked at is the whole annoyance of a flying
    // camera.
    float flySpeed      = 420.0f;
    float flyBoostSpeed = 1600.0f;
    float flyAccel      = 6000.0f;
};

} // namespace body
