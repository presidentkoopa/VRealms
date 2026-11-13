# VR interaction research for VRealms / REMAROTH

Web research only, no code. Written 2026-11-13 for the VRealms lane.

## How to read this document

- **Source-backed** means a review, developer interview or design guide fetched in this session says it. The source is named in the entry and listed at the end. Everything is paraphrased.
- **From memory** marks the few details I know about a game but did not confirm against a fetched page in this session. Treat them as unverified.
- **ROTH mapping** is always **reasoning only, not seen**. Nothing here was tested against the original or our engine.
- The owner's rules put limits on all of it:
  - The flat port comes first, 1:1.
  - Every VR interaction must end in exactly the original's click: examine (trigger) or use (grip).
  - VR adaptations wait until the owner says the flat port is finished.
  - Each mapping is therefore tagged with a phase:
    - **Phase A**: only places or drives the original's own screens and clicks. It adds no new screen, prompt or rule, so it may fit the current rules. That is for the owner to confirm.
    - **Phase B**: a VR presentation or physical action layered on top. It waits for the owner's go-ahead.

### One principle that runs through every category

The ports that kept the original logic and still felt good share one pattern. The physical action is presentation. A single **commit point** in that action fires the original's own event, and the original code decides the outcome.

- **Resident Evil 4 VR (Armature):**
  - The original code keeps running underneath.
  - Teleport fades out and fast-forwards Leon's normal walk instead of moving him directly, so enemies can still interrupt.
  - Changes were made only where they improved VR.
- **Half-Life VR mod:**
  - Buttons are touched and levers grabbed.
  - A "Legacy Use" option falls back to the original use action when a physical interaction misbehaves.
- **Quake VR:** keeps QuakeC gameplay unchanged and rebuilds only the presentation.

For ROTH this means every grab, twist, push or strike resolves to one `examine` or `use` call on the original target, at one moment. Every interactable also keeps a laser plus trigger/grip path that fires the same call directly.

---

## 1. Inventory

### 1.1 Wrist pockets plus over-shoulder backpack (Half-Life: Alyx)
- **How it works:**
  - Each glove has one wrist pocket that holds a single special item (grenade, syringe).
  - Ammo and resin go into a backpack you reach over your shoulder for.
  - The resin count shows on a small display on the glove.
- **What players thought:**
  - The deliberate storing gesture is part of the feel.
  - The pitfall: items handled carelessly fall to the floor instead of going in. Guides warn players to check that resin actually landed in the pack.
- **ROTH mapping (reasoning only):** ROTH's inventory is the original's screen, which stays the inventory. A body pocket would only be a shortcut back to it. Example: dropping the held right-hand item over the shoulder sends the original's "put away / deselect" action.
  - The lesson to take is the pitfall: a key item must never fall to the floor in ROTH, because the original has no such state.
  - **Phase B.**

### 1.2 Backpack pulled over the shoulder with slots (The Walking Dead: Saints & Sinners)
- **How it works:**
  - Reach over the left shoulder to pull the backpack out in front of you.
  - Small items go in inner slots and large ones outside, by drag and drop.
  - Holsters on the hips and shoulder hold weapons.
- **What players thought:**
  - Reviewers liked it.
  - It takes muscle memory, and players accidentally dropped key weapons while learning it.
- **ROTH mapping:** this confirms the owner's choice: a panel brought in front of the player, worked by hand. Don't add a second, invented inventory. **Phase A** for placing the original screen; the gesture to summon it is **Phase B**.

### 1.3 Attaché case floating in front, items lifted for 3D inspection (Resident Evil 4 VR)
- **How it works:**
  - The original grid inventory is kept, raised in front of you like a wall.
  - Pointing at an item lifts it out of the grid for a 3D look.
  - You grab and rotate items into place by hand instead of using buttons.
  - A countertop holds items temporarily while you rearrange.
- **What players thought:**
  - Handling familiar items in 3D was a highlight.
  - The discard button was easy to hit by mistake, and items were lost with no undo.
- **ROTH mapping:** this is the closest precedent for "the original's own screen floating in front of the player." The original layout stays as it is, and only the input changes to touch or point.
  - **Lesson:** keep the original's destructive actions (if ROTH has any) hard to trigger by accident.
    - Require trigger or grip as a deliberate press while pointing at them.
    - Never let them fire on a passing finger.
  - **Phase A.**

### 1.4 Wrist-attached panels and detachable menus (Rec Room, No Man's Sky, Toybox, From Other Suns, survey article)
- **How it works:** the menu lives on the off-hand wrist and is worked by pointing or touching with the other hand. Rec Room lets you detach the panel so it can be used one-handed.
- **What players thought:** quick to reach and intuitive. The survey authors say a wrist screen suits sci-fi and can feel out of place in grounded settings.
- **ROTH mapping:** a wrist screen would be an invented device in a 1996 occult house, so it is a poor fit.
  - The useful part is detachability. A panel that stays where you leave it (world-locked) helps one-handed and seated players.
  - **Phase A** if it is only where the original screen sits.

### 1.5 Menus that take both laser and direct touch (Half-Life: Alyx; Meta guidance)
- **How it works:**
  - Alyx's menus accept a laser pointer or a fingertip poke.
  - Meta's guidance:
    - Direct touch for near content.
    - Rays for content out of reach, so a seated player can stay relaxed.
    - The cursor marks the exact hit point.
- **ROTH mapping:** this matches the decided scheme of finger touch, or laser plus trigger and grip. Both paths must produce the same original click on the same hotspot. **Phase A.**

### 1.6 Touchscreen feel without a real surface (Lone Echo)
- **How it works:**
  - Ready at Dawn let the finger push a little past a button's surface (an over-press distance) to make up for the missing physical resistance.
  - Buttons react with a visible jump, and sliders keep a simple bit of momentum.
  - Their legibility findings:
    - Text needed to be about 1.75 times print size and bolder.
    - Light text on a mid-dark background read best.
    - Text drawn through geometry caused eye strain.
- **ROTH mapping:** if the inventory is poked with a finger, give each original hotspot an over-press depth and fire the click on crossing it, not on first contact.
  - Do not restyle the original screen.
  - Scaling the whole panel (distance and size) is the legitimate way to make the original's small font readable.
  - **Phase A** for depth and size; **Phase B** for any added visuals.

### 1.7 Throwing items "into" storage (The Gallery: Call of the Starseed; MADiSON VR)
- **How it works:** you store items by tossing them into a backpack, or over your shoulder in MADiSON VR.
- **What players thought:** it is quick, but MADiSON VR was criticised overall for awkward contextual controls.
- **ROTH mapping:** a possible Phase B shortcut for "put the held item away." It must map to the original's own action and never create a world drop.

---

## 2. Examining items in the hand

### 2.1 Lift-to-inspect, then put back (RE4 VR; Myst; The Room VR)
- **How it works:**
  - RE4 VR lifts the pointed item out of the grid for 3D inspection.
  - In Myst VR objects snap back to their anchor when released.
  - In The Room VR small objects (keys, artefacts, lockpicks) are handled one at a time, and small boxes open to reveal what is inside.
- **What players thought:**
  - Inspecting in 3D was enjoyed.
  - Myst's snap-back broke immersion when players tried to pass an item from one hand to the other.
- **ROTH mapping:** ROTH's examine is a close-up film plus text.
  - Trigger on the held item (or on an inventory icon) is the original examine click.
  - The film can play on a panel attached to the item or the hand, at reading distance, frame for frame as the original shows it.
  - Since the held item lives in the right hand by design, hand-to-hand passing is not needed, which removes Myst's complaint.
  - **Phase A** for playing the original film on a placed panel; **Phase B** for anything 3D.

### 2.2 Reading documents up close (Dead Secret; MADiSON VR; Myst; Room VR)
- **What players thought:**
  - **Dead Secret:** text was crisp and easy to read, and leafing through documents when lost felt natural. The reviewer still noted that reading in VR is not always comfortable.
  - **MADiSON VR:** clue photos that the flat game showed large and squared-up appeared small and tilted in VR. Players had to grab-pull them to the face, sometimes several times. This was a top complaint.
  - **The Room VR:** reviewers mentioned eye strain from fine symbols and text.
  - **Myst VR:** reading physical books in the library felt more natural than clicking through on-screen text.
- **ROTH mapping:** show the original's document screen squared to the eyes at a fixed comfortable distance, and don't leave it as a small tilted prop in the hand.
  - The hand can "hold" the document as presentation.
  - The readable view is the original page, sized to be legible.
  - **Phase A** (placement of the original screen).

### 2.3 Turning pages: move the page, not the reader (academic study, Aalborg)
- **What the study found:** in a study with 18 participants, moving pages beat moving the camera. It was more comfortable and gave faster, more fluent reading. A linear page row helps focus on one page, while a radial layout helps jumping between pages.
- **ROTH mapping:** where the original pages through a document, page turns should swap the content in place in front of the reader. Never move the player's view. **Phase A.**

### 2.4 Hidden contents revealed by a viewing tool (The Room VR goggles; Red Matter scanner)
- **How it works:**
  - **The Room VR:** spirit goggles reveal hidden writing and puzzles, and turn found notes into ghostly replayed scenes. Players toggled them constantly.
  - **Red Matter:** a single scanner translates the in-world script so you can read it, and also copies data. Reviewers praised how much this one tool carries.
- **ROTH mapping:** see 5.4, the Magical Torch. For examine, the takeaway is that one tool that reveals is easy to learn. **Phase B.**

### 2.5 Books that open in the hands (Myst VR library)
- **How it works:** Myst's books are physical objects you pick up and read. Some hold clues and diagrams.
- **ROTH mapping (the owner's idea):**
  - A book opens in the hands as presentation.
  - The key inside appears only when the original's examine or use on the book would produce it.
  - The open/close animation must not be a gate the original doesn't have.
  - **Phase B.**

---

## 3. Using an item on the world

### 3.1 Keys and items into sockets (Syberia VR; The Room VR; Silent Slayer; The Midnight Walk)
- **How it works:**
  - **Syberia VR**, a VR remake of the 2002 point-and-click: you twist knobs, slide levers and insert keys by hand. The reviewer found this tactile and fun when it worked.
  - **The Room VR:** puzzles of the "item A opens receptacle B" kind, layered later.
  - **Silent Slayer:** you turn keys and trace runes on the coffin.
- **What players disliked:**
  - **Syberia VR:** a key fell out of the world and could not be inserted, which soft-locked the game.
  - **The Midnight Walk:** keys are dropped in front of the keyhole instead of being handled. The reviewer counted this as a compromise.
- **ROTH mapping:** the held right-hand item near a valid target shows a socket (a ghost of the item at the target).
  - Release or grip into the socket is the commit: one original `use item-on-target` click.
  - The key can then play a short insert-and-turn animation as presentation.
  - Two rules follow from the bugs above:
    - The item never becomes a free physics object that can be lost.
    - On release outside a socket, it goes back to the hand or the inventory.
  - **Phase B** (the socket visual); the click itself is Phase A.

### 3.2 Let the game finish a hard motion (I Expect You To Die)
- **How it works:** Schell Games found that a circular motion to unscrew a screw failed. Holding the screwdriver to the screw and letting it unscrew automatically worked. Their lesson is to be honest about what the interface can sense.
- **ROTH mapping:** after the commit, the rest of the physical action can animate by itself. Use this for keys turning, valves finishing their turn, and drawers sliding to the original's open state. **Phase B.**

### 3.3 Combining two items
- **Source-backed:**
  - RE4 VR lets you physically handle and arrange items in its grid inventory.
  - The Walking Dead: Saints & Sinners crafts at a bench with one "build" press after you research a recipe.
  - I found no strong precedent where combining two items by touching them together in mid-air was praised in a narrative adventure.
- **ROTH mapping:** keep combining on the original inventory screen, done the way the original does it (pointing or touching the two items in its own order). That is guaranteed 1:1.
  - A Phase B option is to bring the held item to an inventory icon, which fires the same combine click the original would.
  - Reasoning only: free-air combining risks false triggers and an invented "combine" state the original never had.

### 3.4 Pointing use as a first-class path (Colossal Cave as a warning; Half-Life VR "Legacy Use")
- **How it works:**
  - Colossal Cave's 3D/VR remake kept the original puzzles and solutions. It replaced the text verbs with a cursor from one controller and floating "use/drop" buttons.
  - The reviewer found this disconnected and unintuitive, gave up and went back to the text version.
  - The HL VR mod keeps an original-use fallback behind the physical interactions.
- **ROTH mapping:** in the owner's scheme the laser is one of two ways in, not the only one. Floating verb buttons would be invented UI and should never be added.
  - The laser must target the same hotspots the original's cursor would.
  - Trigger or grip on them is the examine or use click.
  - **Phase A.**

---

## 4. Doors, drawers, cabinets, levers, switches, valves, buttons

### 4.1 Physical doors with knobs that can be partly open (RE4 VR)
- **How it works:** Armature made doors physical, with knobs and hinges, so they can stand partly open. Enemies had to learn about that state. The team called it worth the effort.
- **ROTH mapping:** a half-open state the original does not have would change the logic, because the original's AI and collision only know open or closed.
  - Make the door grab-and-swing presentation that commits to the original's single "use door" click past a threshold, for example about 20 degrees.
  - The swing then completes (or reverses) to the original state.
  - **Phase B.**

### 4.2 "Norman doors" and doors you can't read (Wraith: The Oblivion – Afterlife)
- **What players disliked:** every swinging door looked the same from both sides, so players could not tell push from pull, which was frustrating in chases.
- **ROTH mapping:** if a door is grabbed, accept either push or pull, whichever direction the original door actually opens. The knob grab alone is the commit. **Phase B.**

### 4.3 Hand lands near the handle, not on it (Phasmophobia)
- **What happened:**
  - In August 2026 Kinetic replaced the whole VR rig and fitted the model's arms to the player's arm length.
  - With fixed model arms the estimated elbow and shoulder put the hand near objects instead of on them.
  - Half of that patch's VR fixes were door fixes: grab reticle, tent-door hand animations, doors grabbed from behind.
  - An earlier OpenXR overhaul added a small grab cursor and a choice between toggle and hold grab.
- **ROTH mapping:** this is relevant to the owner's IK body.
  - For door and drawer grabs, drive the grab from the tracked hand pose, not from an IK arm.
  - Give each handle a generous grab volume and an authored FBX finger pose.
  - Offer toggle or hold grip as a setting.
  - **Phase B.**

### 4.4 Single grab point makes objects fumbly (Thief VR: Legacy of Shadow)
- **What players disliked:** most objects can be handled, which was praised, but each has one grab point in the middle. Players "pawed" at things, and pulling the lockpick out of inventory was hit or miss when hurrying.
- **ROTH mapping:** author grab points per handle, knob and lever, with the matching finger pose from the FBX set. **Phase B.**

### 4.5 Kinematic constraint, not free physics (Meta grab guidance; Alyx)
- **Source-backed:**
  - Meta recommends constraining levers and drawers to their axis.
  - It warns that physics objects lag when the hand moves fast and that kinematic objects can pass through geometry.
  - In Alyx a drawer can close on your (invisible) arm because the arm exists only for physics. Valve chose not to show arms, since wrong arms "stand out an enormous amount."
- **ROTH mapping:**
  - Drawers and cabinets become constrained movers that follow the hand along one axis.
  - Past a threshold they commit the original click, then animate to the original's state.
  - Use no rigid-body physics on anything the logic depends on.
  - **Phase B.**

### 4.6 Buttons pressed by any finger or knuckle (Myst VR hand tracking)
- **How it works:**
  - Cyan put capsule colliders between finger joints so any finger or knuckle presses a button.
  - Grabbable things (valves, doors, book pages) reuse the controller grab: the hand turns orange on hover, and a fist grabs.
  - Locomotion is suppressed within about 25 cm of an interactable. They tried 60 cm, which blocked needed movement; below 25 cm caused accidental moves.
- **ROTH mapping (the owner's "switches you hit"):**
  - A switch's original "use" fires when any hand collider crosses its press depth.
  - A 25 cm interaction zone that suppresses stick movement is a cheap safety net.
  - **Phase B** (the physical hit); the click is the original's.

### 4.7 Heavy levers and valves (Red Matter; Myst VR; Riven)
- **How it works:** Red Matter's levers feel weighty and sometimes need two hands. Moving too fast loses your grip.
- **What players disliked:**
  - Riven's interactions were praised for physicality.
  - They were also criticised: players often couldn't tell whether they had pressed something correctly, or whether it was interactive at all.
- **ROTH mapping:**
  - Never require two hands for a 1:1 click (see one-handed play).
  - Make the commit unmistakable: a click sound, a haptic pulse, and the original's own result.
  - **Phase B.**

### 4.8 Cosmetic choices that cost immersion (Hitman 3 VR)
- **What players disliked:** doors, switches and pickups stayed as button presses, so "your head is in VR but your body is outside." Scopes and cameras fell back to a 2D screen, which was jarring.
- **ROTH mapping:** this argues for the owner's direction, where physical ways are the goal, while still keeping the button/laser route as a fallback. **Phase B.**

---

## 5. Light and darkness

### 5.1 Hand-aimed light (Half-Life: Alyx)
- **How it works:** Valve moved the flashlight off the head and into the hand, and called this a VR-specific improvement. Darkness is a main source of fear because the light is aimed independently of the gaze. The Jeff section deliberately overloads the hands: flashlight, multitool, door, bottles, covering the mouth.
- **ROTH mapping (the owner's "lamp carried as a real light"):**
  - The lamp's dynamic light follows the hand that holds it.
  - Whatever the original's lamp changes in game state (light level, visibility) stays exactly as the original computes it.
  - Only the beam's direction and shadows are presentation.
  - **Phase B.**

### 5.2 Small light radius that guides (Lies Beneath)
- **How it works:** in the opening blizzard a lighter is the only light and reaches a foot or two. Its embers drift toward the way forward.
- **What players disliked:** the fog-limited start felt samey.
- **ROTH mapping:** any Phase B guidance must not invent hints where the original shows none, so ember guidance is out. The tight radius itself is fine as presentation.

### 5.3 Lighting candles (Phasmophobia; The Midnight Walk)
- **How it works:**
  - **Phasmophobia:** you hold the candle and the lighter, light it, then place it. Candles matter because staying in the dark lowers sanity.
  - **The Midnight Walk:** striking matches with motion controls was praised as physical and the reason VR is the best way to play.
- **What players disliked:** candle puzzles with a hidden timer, where a whole set must be lit, were tough and fiddly with the match launcher.
- **ROTH mapping (the owner's idea):**
  - Strike the match on the matchbox by hand: a box-side collider plus a speed threshold.
  - Touch the flame to a wick. That contact is the commit, and it fires the original's `use matchbox on candle` click.
  - Rules:
    - No timers that the original doesn't have.
    - A generous wick volume.
    - The match never burns out before the first wick commits.
  - **Caution (reasoning only):**
    - If the original lights a candle group with one click, wick-by-wick lighting is pure presentation that plays out after that click.
    - The light level the logic sees must equal the original's at every tic.
    - The "darkness pushed back" effect should be renderer-only, converging on the original's lighting state.
  - **Phase B.**

### 5.4 A beam that reveals hidden things (Phasmophobia UV; The Room VR goggles; The 7th Guest VR spirit lantern; Thief VR glyph vision)
- **How it works:**
  - **Phasmophobia:** a UV light shows evidence such as prints, and UV charge lets you see them for a few seconds after the light is put away.
  - **The Room VR:** goggles reveal hidden messages and puzzles.
  - **The 7th Guest VR:** the spirit lantern repairs broken planks and materializes hidden objects, alongside a "spirit board" map with hints.
  - **Thief VR:** glyph vision briefly highlights important objects and hidden areas.
- **ROTH mapping (the owner's "Magical Torch beam revealing hidden wards"):**
  - Render the original's hidden wards only inside the beam's cone, as a renderer mask.
  - Whatever the original's torch use changes in game state still happens only on the original's click.
  - Show only wards that exist in the original data. The 7th Guest-style hint board would be an addition and is out.
  - **Phase B.**

### 5.5 A light as a limited resource (Wraith: The Oblivion – Afterlife)
- **How it works:** the camera flash doubles as a light and as the only weapon. It runs on a refillable resource, so players chose to explore in the dark to save it.
- **ROTH mapping:** only where the original already has fuel or charge. Don't add one. Reference only.

### 5.6 Comfort in the dark
- **Source-backed:**
  - On OLED headsets, true black makes darkness look real.
  - Mura (fixed-pattern noise) is most visible in fades, transitions and low-detail scenes, which horror uses a lot.
  - Schell Games' rules: don't move the camera without the body, don't accelerate it, and keep the horizon level.
- **ROTH mapping:**
  - Keep a small ambient floor in very dark areas, or offer it as a gamma setting.
  - Prefer cuts to slow fades in transitions.
  - Never move the camera without the body.
  - **Phase B** (settings).

---

## 6. NPC dialogue choice

### 6.1 Dialogue tree that opens when you approach (Down the Rabbit Hole)
- **How it works:** getting close to the Cheshire Cat opens a dialogue tree with a few options.
- **ROTH mapping:** the original's dialogue box opens on the original's trigger, not on proximity. Proximity opening would change behaviour. Reference only.

### 6.2 Touch-panel choices (Lone Echo)
- **How it works:** Lone Echo's interface is built on direct index-finger touch with over-press depth (see 1.6). Its legibility lessons apply to any text panel.
- **ROTH mapping:**
  - Show the original's dialogue box as a world-locked panel near the speaker, at reading distance and squared to the eyes.
  - Its options are picked by finger touch or by laser plus trigger, each firing the original's selection click.
  - Same box, same layout, same text wrapping.
  - **Phase A** (placement).

### 6.3 Subtitle placement in VR (survey of 20 VR apps, Lancaster University)
- **Source-backed:**
  - Head-locked subtitles are most common and always visible. They are hard to read if they sit at the edge of view; Skyrim VR and Fallout 4 VR let them drift out.
  - World-locked subtitles only show when their anchor is in view.
  - Subtitles attached to the speaker indicate who is talking.
  - Adaptive-depth subtitles avoid occlusion but jitter and move while you read.
  - Text drawn through nearer objects creates a depth conflict.
- **ROTH mapping:**
  - Keep the original's subtitle exactly as the original draws it, with no re-wrapping (the owner's 2026-11-12 rule).
  - Its only VR choice is where the panel sits.
  - Fixed depth, near the speaker, never drawn through geometry, never jittering.
  - **Phase A** (placement only).

### 6.4 Gaze and gesture (Myst VR hand tracking; The Under Presents)
- **Source-backed:**
  - Myst's hand tracking chose a pinch for menus because players already learn it from the headset's system gesture.
  - In The Under Presents players can't speak and communicate with finger snaps and broad gestures.
- **ROTH mapping:** gaze or gesture dialogue selection would be a new input, so keep it Phase B at most. The owner's scheme (touch or laser plus trigger/grip) already covers it.

---

## 7. Cutscenes and full-motion video

### 7.1 Fixed screen as a comfort option (Resident Evil Village VR)
- **How it works:** scripted scenes play in full first-person VR by default, with the camera moving Ethan around. A 2D-screen option exists for comfort.
- **What players thought:**
  - Reviewers called the immersive scenes highlights.
  - They also noted bugs in how the game steers the player's gaze, and that forced movement is hard for motion-sensitive players.
- **ROTH mapping:** ROTH's FMV is flat video, so there is no camera to move.
  - Play it on a large world-locked screen in front of the player, with the world dimmed or held still behind it.
  - Never head-lock the video.
  - The player's head stays free; the video does not follow it.
  - **Phase A** (placement of the original video).

### 7.2 Original videos kept as an option (Myst VR "Hands & More" update)
- **Source-backed:** the update's title lists "Classic Videos" next to hand tracking and an in-game journal. I could not confirm from the page how they are presented.
- **ROTH mapping:** this supports showing ROTH's own FMV unchanged and not re-staging it. **Phase A.**

### 7.3 Volumetric actors in the room (The 7th Guest VR)
- **How it works:** the 1993 FMV became volumetric captures of actors, played as misty ghost scenes in the house. They are triggered by puzzle progress, for example a gramophone record in the foyer.
- **What players thought:** reviewers felt their presence mattered to the story.
- **ROTH mapping:** ROTH has only flat film, and re-shooting is impossible. A possible Phase B presentation is to project the original FMV inside the house: on a wall, through a doorway, or as a framed apparition. Same frames, same timing, same trigger.

### 7.4 Comic panels floating in the world (Lies Beneath)
- **How it works:** each chapter opens with a large floating comic page, and panels act as signposts inside levels.
- **What players disliked:** on Quest, performance stuttered when approaching narration panels.
- **ROTH mapping:** keep video decode off the main simulation path so FMV start never hitches the sim. **Phase A** (engineering).

### 7.5 Don't fall back to flat for gameplay views (Hitman 3 VR)
- **What players disliked:** scopes and camera lenses switched to a 2D cinema screen, which reviewers found jarring.
- **ROTH mapping:**
  - Cinema-screen FMV is fine for passive cutscenes.
  - ROTH's examine close-up films (2.1) should sit next to the item or the inventory, not take over the whole view.

---

## 8. Ports and VR remakes of old games

| Game | What worked | What fans disliked | Lesson for ROTH (reasoning only) |
|---|---|---|---|
| **Myst** (VR remake) | Sticks close to the 1993 original; reading physical books; positional audio along pipes; full comfort options; seated/standing with height detection | Snap-back of objects; no in-game notes at launch; slow-walk/fast-run gap | Fidelity is valued; inventory and notes friction hurts |
| **Riven** (2024) | Grabbing and turning devices makes contraptions feel real; no map markers, as in the original | Interactables hard to spot in 3D (the fixed frames of the original had framed them); unclear whether an action did anything; no practical note-taking | Every commit needs unmistakable feedback; findability must come from the original's own cues |
| **Syberia VR** (2002 remake) | Hands-on knobs, levers, keys; faithful art and tone | Keys falling out of the world; soft-locks; slow movement; snap-only turn | Physical items that carry logic must not be physics-loose |
| **The 7th Guest VR** | Volumetric actors; spirit lantern; easier puzzles | A few puzzles tedious | It is a reimagining, not 1:1; take presentation ideas only |
| **Colossal Cave** | Original puzzles and points kept | Cursor and floating verb buttons felt disconnected; reviewer returned to text | Don't make VR input a menu of verbs |
| **Resident Evil 4 VR** | Original code underneath; teleport that fast-forwards the original walk; doors, levers made physical; typewriter typed by hand | Discard button mis-hits | The best model for "VR action resolves to original event" |
| **Half-Life VR mod** | Touch buttons, grab levers, climb ladders; Classic Mode | Some physical interactions break, hence "Legacy Use" | Ship a fallback per interactable |
| **Quake VR** | Keeps QuakeC; holsters, physical melee, weapon grip "sticky" option | — | Gameplay frozen; presentation rebuilt |
| **Tomb Raider (Team Beef)** | Switchable views (third person, first person, diorama); IK arms | Head bob can't be turned off; controls stiff | Anything added must be switchable off |
| **MADiSON VR** | Instant camera you raise and wiggle to develop | Small tilted clue photos; floor items need reaching; palm-up icon to act; later had to add magnetic grab | Flat-game reading and reach don't transfer; plan for them |
| **Hitman 3 VR** | — | Buttons for doors and items; 2D fallbacks | Physical where it matters |
| **Doom/Quake ports (QuestZDoom, Doom 3 Quest)** | Large fan following for faithful ports | Not researched in depth this session | — |

Two more facts from these ports:

- **The Room VR (Fireproof):**
  - Touch design didn't translate as easily as expected.
  - Objects needed weight and resistance so hands didn't "flap uselessly."
  - The head is the camera, so the team guided attention with sparkle, not camera moves.
  - The team warned against simplifying puzzles because players got stuck.
- **The Room VR as a game:** reviewers noted hands clipping through walls, and that haptics were the only cue for touching something solid.

---

## 9. Comfort and accessibility (seated, standing, one hand, left hand, reach)

### 9.1 Single controller and seated play (Half-Life: Alyx)
- **Source-backed:**
  - Alyx shipped with:
    - a single-controller mode, "adapting the game to anyone who can only operate one controller";
    - height adjustment;
    - crouch and stand toggles for seated players;
    - subtitles in 10 languages.
  - Valve changed puzzles after seated playthroughs found unreachable elements.
  - Valve designed for small play spaces.
  - One-handed players can wear gas masks instead of covering the mouth.

### 9.2 Automatic one-handed mode and height handles (Cosmonious High, Owlchemy)
- **How it works:**
  - The whole game is playable with one hand, and the mode turns on automatically when only one controller is in use.
  - Every table has a handle to adjust its height.
  - Interactions work at any height.
- **ROTH mapping:** the owner's scheme puts the held item in the right hand.
  - In a one-handed mode the same hand must also reach the inventory panel by laser and trigger/grip. A world-locked panel needs no second hand.
  - A height handle on the floating inventory panel is cheap.
  - **Phase B** (settings).

### 9.3 Handedness
- **Source-backed:** MADiSON VR offers a dominant-hand choice.
- **ROTH mapping:** the default stays right-handed, as decided. A mirror setting swaps held-item hand and laser hand without changing any click. **Phase B.**

### 9.4 Reach (MADiSON VR; Thief VR; Red Matter)
- **What players disliked:**
  - MADiSON VR: floor items needed crouching and stretching, so a magnetic grab was added later.
  - Thief VR: reaching was hard with artificial crouch, so seated play was worse.
  - Red Matter: seated mode left some objects slightly out of reach.
- **ROTH mapping:**
  - Any target the original can click should also be reachable by laser at any height.
  - A short-range "pull to hand" on pickups (Alyx's gravity gloves model, below) is an option.
  - **Phase B.**

### 9.5 Selection that doesn't jump (Alyx gravity gloves)
- **How it works:**
  - Three stages, each with its own sound:
    1. Selection: the target glows.
    2. Lock-on: the trigger locks the target so it can't switch.
    3. Confirmation: a flick pulls the item.
  - Highlights are subtle lights, not yellow outlines.
- **ROTH mapping:** for laser targeting among close hotspots (inventory slots, dialogue lines, small switches), lock the target on trigger-down and fire the original click on release. That prevents mis-clicks. **Phase A.**

### 9.6 Hand-tracking gestures and dead zones (Myst VR)
- **Source-backed:**
  - Turning by wrist rotation failed for palm-down players, so Myst switched to a thumbs-up turn with a dead zone and a reset to center.
  - Movement is suppressed within 25 cm of an interactable.
  - Tracking loss misread fingers, so tolerance was added.
  - Players are reminded to play in a well-lit room.
- **ROTH mapping:** relevant only if bare-hand tracking is supported later. The capacitive-touch FBX finger poses already give finger feedback with controllers. **Phase B.**

### 9.7 General comfort list seen across the games reviewed
- Comfort options seen:
  - snap and smooth turning;
  - teleport and smooth movement;
  - vignette strength;
  - head-bob off (missed in Tomb Raider);
  - seated or standing mode with height offset;
  - subtitle size, colour and background (Midnight Walk: five sizes, two colours, three opacities);
  - toggle or hold grip (Phasmophobia);
  - "sticky" grip (Quake VR).
- Most of these touch only presentation and settings. They must never be written into the owner's shared `doomxr.ini` from the engine or from tests.

---

## Ranked top 15 for VRealms

Effort: S = days, M = one to two weeks, L = more. These are relative estimates (reasoning only) and assume the existing hand and IK work.

| # | Idea | From | ROTH mapping (one line) | Phase | Effort |
|---|---|---|---|---|---|
| 1 | **Commit-point layer: every physical interaction resolves to one original examine/use click, with a laser + trigger/grip fallback on every interactable** | RE4 VR, HL VR mod "Legacy Use", Quake VR | One code path per click, many ways to reach it; guarantees 1:1 | A | M |
| 2 | **Original inventory screen as a world-locked panel, touch and laser both, with over-press depth** | RE4 VR case, Alyx menus, Lone Echo | Original layout untouched; only placement, scale and input change | A | M |
| 3 | **Target lock-on: highlight, lock on trigger/grip down, fire on release; subtle glow, sound and haptic per stage** | Alyx gravity gloves; Riven/Room complaints about unclear feedback | Prevents mis-clicks between close hotspots and makes every commit unmistakable | A | S |
| 4 | **Examine in hand: trigger on the held item plays the original close-up film and text on a panel by the item, squared to the eyes** | RE4 VR lift-to-inspect; MADiSON tilted-photo complaint; Dead Secret | Exactly the original examine, placed for legibility | A | S |
| 5 | **FMV on a large world-locked screen, head free, world held still; decode off the sim path** | RE Village 2D option, Myst classic videos, Lies Beneath stutter | Original frames and timing; no camera motion | A | S |
| 6 | **Dialogue box and subtitles world-locked near the speaker at fixed depth, original layout, touch/laser selection** | Lone Echo, Lancaster subtitle survey | Same box and wrapping; only placement | A | S |
| 7 | **Item sockets for use-item-on-target: held item ghosts into the target; release commits; the item is never physics-loose** | Syberia VR (and its lost-key bug), The Room VR, IEYTD auto-finish | One original use click; insert/turn plays after | B | M |
| 8 | **Switches and buttons hit by any finger/knuckle collider, with a 25 cm movement-suppression zone** | Myst VR | Press depth crossing = original use click | B | S |
| 9 | **Drawers, cabinets, doors as constrained (kinematic) movers with authored FBX grab poses; commit past a threshold, then animate to the original state** | Meta grab guidance, RE4 VR doors, Wraith, Thief VR, Phasmophobia | Either direction accepted; no half-open logic state | B | M |
| 10 | **Lamp carried as a real hand-aimed light** | Alyx flashlight in the hand | Renderer follows the hand; logic light level stays the original's | B | M |
| 11 | **Magical Torch beam as a renderer mask that reveals the original's wards** | Phasmophobia UV, Room VR goggles, 7th Guest lantern, Thief glyph vision | Shows only wards in the original data; state changes only on the original click | B | M |
| 12 | **Candles: strike the match on the box, touch flame to wick = original use click; no timers; darkness pushback is renderer-only and converges on the original state** | The Midnight Walk, Phasmophobia | Watch for one-click-lights-group in the original | B | M |
| 13 | **Accessibility pack: one-handed mode (auto when one controller), handedness mirror, seated height offset and panel height handle, toggle/hold grip, laser reach to any target** | Alyx, Cosmonious High, MADiSON, Phasmophobia, Thief VR | All clicks reachable by one hand, at any height | B | M |
| 14 | **No item ever drops to the floor: release outside a socket returns it to hand or inventory; put-away gesture maps to the original's deselect** | Alyx resin falling, S&S dropped weapons, Syberia lost key, RE4 discard mis-hit | The original has no dropped-item state | B | S |
| 15 | **Books opened in the hands; page turns swap content in place; the key appears only on the original's click** | Myst VR library, Aalborg page-turning study | Presentation over the original examine/use | B | M |

Honourable mention: **items as 3D models built from the game's own close-up films** (the owner's later idea). It fits RE4 VR's praised 3D inspection, but it is presentation that must match the films. Effort L.

---

## Sources

### Inventory, interaction, design guides
- Road to VR, "VR Design Unpacked: The Incredible Details of Half-Life: Alyx" (pages 1 and 2): https://www.roadtovr.com/these-details-make-half-life-alyx-unlike-any-other-vr-game-inside-xr-design/ and https://www.roadtovr.com/these-details-make-half-life-alyx-unlike-any-other-vr-game-inside-xr-design/2/?amp=1
- Game Developer, "Intuitive inventory systems in VR games": https://gamedeveloper.com/design/intuitive-inventory-systems-in-vr-games
- GameSpot, Half-Life: Alyx weapon upgrades and items guide: https://www.gamespot.com/articles/half-life-alyx-weapon-upgrade-and-items-guide/1100-6475048/
- PCGamesN, Half-Life: Alyx VR design: https://www.pcgamesn.com/half-life-alyx/vr-design
- UploadVR, Valve interview on Half-Life: Alyx: https://uploadvr.com/valve-interview-half-life-alyx/
- Game Informer, Valve on why arms don't work in VR: https://www.gameinformer.com/index.php/interview/2020/03/23/valve-talks-half-life-alyx-and-why-arms-dont-work-in-vr
- Road to VR, Half-Life: Alyx accessibility options: https://roadtovr.com/half-life-alyx-accessibility-options-subtitles-seated-single-controller/
- Meta for Developers, "Rebuilding Resident Evil 4": https://developers.meta.com/horizon/blog/rebuilding-resident-evil-4-what-it-took-to-bring-capcoms-classic-to-oculus-quest-2/
- HyperX blog, RE4 VR inventory: https://blog.hyperx.com/article/12284/resident-evil-4-vr-makes-inventory-organization-more-enjoyable-than-ever
- Road to VR, The Walking Dead: Saints & Sinners review: https://www.roadtovr.com/the-walking-dead-saints-sinners-review-vr/
- Meta for Developers, grab usage guidance: https://developers.meta.com/vr/design/grab_usage/
- Meta for Developers, indirect (ray) interactions guidance: https://developers.meta.com/vr/design/indirect_interactions_usage/
- Road to VR, Designing Lone Echo's virtual touchscreen interfaces (page 2): https://roadtovr.com/designing-lone-echo-echo-arena-virtual-touchscreen-interfaces-robert-duncan/2/
- Schell Games, "Making Great VR: Lessons Learned From I Expect You To Die": https://schellgames.com/blog/making-great-vr-six-lessons-learned-from-i-expect-you-to-die

### Puzzle and adventure games
- God is a Geek, The Room VR: A Dark Matter review: https://www.godisageek.com/reviews/the-room-vr-a-dark-matter-review
- Road to VR, The Room VR preview: https://roadtovr.com/preview-the-room-a-dark-matter-vr/
- PlayStation Blog, how Fireproof brought The Room to PS VR: https://blog.playstation.com/?p=222489
- Road to VR, Myst for Quest review: https://www.roadtovr.com/myst-vr-quest-review/
- Road to VR, hand tracking in Myst: https://roadtovr.com/design-implementation-of-hand-tracking-in-myst-oculus-quest/
- Meta blog, Myst "Hands & More" update: https://www.meta.com/blog/myst-hands-more-update-adds-hand-tracking-in-game-journal-classic-videos-and-more/
- Road to VR, Riven review: https://roadtovr.com/riven-review-quest-2-3-steam-vr/
- UploadVR, Riven VR review: https://www.uploadvr.com/riven-vr-review/
- UploadVR, Syberia VR review: https://www.uploadvr.com/syberia-vr-review/
- UploadVR, Colossal Cave review: https://uploadvr.com/colossal-cave-review/
- Room Escape Artist, The 7th Guest VR review: https://roomescapeartist.com/2024/02/06/7th-guest-vr-review/
- Road to VR, The 7th Guest VR hands-on: https://roadtovr.com/?p=171740
- UploadVR, Shadow Point review: https://uploadvr.com/shadow-point-review/
- Road to VR, Down the Rabbit Hole hands-on: https://roadtovr.com/down-the-rabbit-hole-gamescom-2019/
- UploadVR, Silent Slayer: Vault of the Vampire review: https://uploadvr.com/silent-slayer-vault-of-the-vampire-review
- Road to VR, Dead Secret review: https://roadtovr.com/?p=36910
- Road to VR, Red Matter review: https://roadtovr.com/red-matter-review/
- GameSpot, Moss review: https://Gamespot.com/reviews/moss-review-tiny-triumph/1900-6416866/
- No Proscenium, The Under Presents review: https://noproscenium.com/the-under-presents-conjures-magic-in-virtual-reality-review-b2e23be4a1b9

### Horror games
- Road to VR, Wraith: The Oblivion – Afterlife review: https://www.roadtovr.com/wraith-oblivion-afterlife-review-quest-rift/
- UploadVR, Lies Beneath review: https://www.uploadvr.com/lies-beneath-review/
- UploadVR, Resident Evil Village PSVR 2 review: https://uploadvr.com/resident-evil-8-village-psvr-2-review/
- UploadVR, Resident Evil 7 VR review: https://uploadvr.com/resident-evil-7-vr-review/
- UploadVR, MADiSON VR review: https://uploadvr.com/madison-vr-review
- Road to VR, Thief VR: Legacy of Shadow review: https://roadtovr.com/thief-vr-review-quest-3-psvr-2-steamvr/
- UploadVR, The Midnight Walk VR review: https://uploadvr.com/the-midnight-walk-vr-review
- UploadVR, The Exorcist: Legion VR review: https://uploadvr.com/the-exorcist-legion-vr-review/
- Gfinity, Phasmophobia candles: https://www.gfinityesports.com/phasmophobia/phasmophobia-how-to-use-a-candle/
- GINX, Phasmophobia ultraviolet evidence: https://www.ginx.tv/en/ultraviolet-how-to-find-evidence
- Kinetic Games, Phasmophobia VR overhaul notes: https://kineticgames.co.uk/blog/phasmophobia-vr-overhaul
- VR.org, Phasmophobia v0.19 VR rig and door fixes: https://vr.org/articles/phasmophobia-v0-19-vr-character-rig-replaced-doors-2026
- UploadVR, Hitman 3 VR review: https://www.uploadvr.com/hitman-3-vr-review/

### Ports of old games
- Half-Life VR Mod, Steam store page: https://store.steampowered.com/app/1908720/HalfLife_VR_Mod/
- Quake VR (vittorioromeo/quakevr), GitHub: https://www.github.com/vittorioromeo/quakevr
- MIXED, Tomb Raider (Team Beef) on Quest 3 hands-on: https://mixed-news.com/en/tomb-raider-vr-hands-on/

### Accessibility, reading, subtitles, display
- UploadVR, Cosmonious High accessibility update: https://uploadvr.com/cosmonious-high-accessibility-update-adds-one-handed-mode-more-available-now/
- Aalborg University, "Page-Turning Techniques for Reading Interfaces in Virtual Environments": https://vbn.aau.dk/en/publications/page-turning-techniques-for-reading-interfaces-in-virtual-environ
- Lancaster University, "Subtitles in Interactive Virtual Reality: Using Gaze to Address Depth Conflicts": https://eprints.lancs.ac.uk/id/eprint/132411/4/Subtitles_in_Interactive_Virtual_Reality_Using_Gaze_to_Address_Depth_Conflicts.pdf
- UploadVR, PSVR2 technical analysis (OLED black levels, mura): https://uploadvr.com/psvr2-technical-analysis/
- Road to VR, VorpX virtual cinema: https://roadtovr.com/vorpxs-new-virtual-cinema-lets-you-play-non-vr-games-on-a-virtual-big-screen/

### Gaps, not covered by a fetched source this session
- System Shock VR.
- QuestZDoom details.
- How Saints & Sinners selects dialogue lines.
- The exact presentation of Myst's "Classic Videos."
- Any 90s FMV adventure shipped as a flat-video VR port with reviews.
