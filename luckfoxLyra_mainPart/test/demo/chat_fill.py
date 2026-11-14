"""The chat history: a search team's last hour (times relative to now), written into merld's DB."""
import sqlite3
import sys
import time

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import scenario as sc  # noqa: E402

db, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'history'
now = int(time.time())
B = sc.BROADCAST
sos_lat, sos_lon = sc.where(0xC705)
pos_lat, pos_lon = sc.where(0xC706)
if mode == 'history':
    rows = [
        # the day before: getting ready
        (-86400 - 5 * 3600, 'out', sc.OWN, B, 1, 'Завтра о 15:00 пошук у Сосновому Бору, збір біля входу в парк', 'sent'),
        (-86400 - 5 * 3600 + 180, 'in', 0xC702, sc.OWN, 0, 'Буду з двома волонтерами', 'received'),
        (-86400 - 4 * 3600, 'in', 0xC705, B, 1, 'Перевірка зв\'язку перед виїздом: чую всіх', 'received'),
        (-58 * 60, 'out', sc.OWN, B, 1, 'Штаб: початок пошуку, сектори 1–4. Доповідайте кожні 15 хв.', 'sent'),
        (-56 * 60, 'in', 0xC702, B, 1, '0xC702 на позиції, сектор 1, зв\'язок є', 'received'),
        (-55 * 60, 'in', 0xC703, B, 1, 'Сектор 2: рухаємось просікою до озера', 'received'),
        (-47 * 60, 'in', 0xC705, B, 1, 'Сектор 3, група з 4 осіб, все добре', 'received'),
        (-41 * 60, 'out', sc.OWN, 0xC706, 0, '0xC706, підтвердіть свою позицію', 'sent'),
        (-40 * 60, 'in', 0xC706, sc.OWN, 0, f'My position: {pos_lat:.5f}, {pos_lon:.5f}', 'received'),
        (-31 * 60, 'in', 0xC703, B, 1, 'Знайшли речі біля старої вежі, фотографуємо', 'received'),
        (-24 * 60, 'out', sc.OWN, B, 1, 'Прийнято. Сектор 2 — продовжуйте на південь', 'sent'),
        (-12 * 60, 'in', 0xC705, B, 1, f'SOS! Need help at {sos_lat:.5f}, {sos_lon:.5f}, heading 300° NW', 'received'),
        (-11 * 60, 'out', sc.OWN, B, 1, 'Група 0xC702 висувається до 0xC705', 'sent'),
        (-9 * 60, 'in', 0xC702, B, 1, 'Бачимо 0xC705, будемо за 5 хв', 'received'),
        (-3 * 60, 'in', 0xC705, B, 1, 'Все гаразд, травма легка, виходимо до дороги', 'received'),
        (-60, 'out', sc.OWN, 0xC708, 0, '0xC708, як зв\'язок на вашому кінці?', 'sent'),
    ]
else:   # a fresh SOS, for the alarm
    sos2_lat, sos2_lon = sc.where(0xC703)
    rows = [(-8, 'in', 0xC703, B, 1, f'SOS! Need help at {sos2_lat:.5f}, {sos2_lon:.5f}, heading 210° SW', 'received')]
con = sqlite3.connect(db)
last = con.execute("SELECT COALESCE(MAX(id), 0) FROM messages").fetchone()[0]
for i, (dt, d, s, r, bc, text, st) in enumerate(rows, 1):
    con.execute("INSERT INTO messages (id, time, dir, sender, receiver, broadcast, text, status) VALUES (?,?,?,?,?,?,?,?)",
                (last + i, now + dt, d, s, r, bc, text, st))
con.commit()
print(f"{len(rows)} messages")
