import csv, sys
from collections import Counter

ACTION = {0:"None",1:"Attack",2:"PowerAttack",3:"QuickAttack",4:"QuickAttackR",5:"QuickAttackL",
 6:"SimpleWhirl",9:"SprintAttack",10:"WhirlAttack",11:"PierceAttack",12:"JumpAttack",13:"RamAttack",
 14:"HackAttack",15:"FinishingAttack",16:"Parade",17:"ParadeR",18:"ParadeL",19:"ExitParade",
 20:"QuickParadeStumble",21:"ParadeStumble",22:"ParadeStumbleR",23:"ParadeStumbleL",24:"HeavyParadeStumble",
 25:"QuickStumble",26:"Stumble",27:"StumbleR",28:"StumbleL",29:"SitKnockDown",30:"GetUpAttack",
 31:"GetUpParade",32:"LieKnockDown",33:"LieKnockOut",34:"PierceStumble",35:"Die",36:"LieDead"}
# states in which the player has lost control of the character
LOCKED = set(range(20,35)) | {35,36}
ATTACKS = set(range(1,16))

rows=list(csv.DictReader(open(sys.argv[1] if len(sys.argv)>1 else "arena2.csv",encoding="utf-8")))
if not rows:
    print("no samples"); raise SystemExit
def num(r,k,d=0):
    v=r.get(k,"")
    try: return int(float(v))
    except Exception: return d

T=float(rows[-1]["t"]); dt=T/max(1,len(rows)-1)
print(f"duration {T:.1f}s, {len(rows)} samples ({1/dt:.1f} Hz)\n")

hp=[(float(r["t"]), num(r,"hp",-1)) for r in rows if num(r,"hp",-1)>=0]
hits=[(t,prev-cur) for (t,cur),(pt,prev) in zip(hp[1:],hp[:-1]) if cur<prev]
print(f"HP {hp[0][1]} -> {hp[-1][1]}   hits taken: {len(hits)}")
if hits:
    dmg=[d for _,d in hits]
    print(f"  damage per hit: min={min(dmg)} avg={sum(dmg)/len(dmg):.1f} max={max(dmg)}  (max hp {hp[0][1]})")
    print(f"  a max hit removes {max(dmg)*100.0/max(1,hp[0][1]):.0f}% of the health bar")
    if len(hits)>1:
        gaps=[hits[i+1][0]-hits[i][0] for i in range(len(hits)-1)]
        print(f"  interval between hits: min={min(gaps):.2f}s avg={sum(gaps)/len(gaps):.2f}s max={max(gaps):.2f}s")

acts=[num(r,"action") for r in rows]
locked=[a in LOCKED for a in acts]
print(f"\nplayer control: locked {sum(locked)*dt:.1f}s of {T:.1f}s = {100.0*sum(locked)/len(acts):.0f}% of the fight")
runs=[]; cur=0
for l in locked:
    if l: cur+=1
    elif cur: runs.append(cur*dt); cur=0
if cur: runs.append(cur*dt)
if runs:
    print(f"  uninterrupted lock-ups: {len(runs)}  longest {max(runs):.2f}s  avg {sum(runs)/len(runs):.2f}s")
    free=[]; cur=0
    for l in locked:
        if not l: cur+=1
        elif cur: free.append(cur*dt); cur=0
    if free: print(f"  free windows between lock-ups: shortest {min(free):.2f}s avg {sum(free)/len(free):.2f}s")
print("\nplayer action histogram:")
for a,c in Counter(acts).most_common(8):
    print(f"  {ACTION.get(a,a):<20} {c*dt:5.1f}s  {100.0*c/len(acts):4.0f}%")
foes=[num(r,"foes") for r in rows]
print(f"\nenemies within 900: max {max(foes)}  avg {sum(foes)/len(foes):.1f}")
fa=Counter()
for r in rows:
    for v in (r.get("foe_actions") or "").split(";"):
        if v.strip().isdigit(): fa[int(v)]+=1
if fa:
    print("enemy action histogram:")
    for a,c in fa.most_common(6):
        print(f"  {ACTION.get(a,a):<20} {c}")
