from pathlib import Path
import json
from playwright.sync_api import sync_playwright
base=Path(__file__).resolve().parent
report_dir=base/'deliverables' if (base/'deliverables'/'研究报告.html').exists() else base
p=report_dir/'研究报告.html'
with sync_playwright() as pw:
 b=pw.chromium.launch(executable_path='/usr/bin/chromium',headless=True,args=['--no-sandbox'])
 page=b.new_page(viewport={'width':1440,'height':1000},device_scale_factor=1)
 errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
 page.set_content(p.read_text(), wait_until='load');page.wait_for_function("document.querySelectorAll('figure img').length===4 && [...document.images].every(i=>i.complete && i.naturalWidth>0)")
 page.screenshot(path=str(base/'report-desktop-preview.png'),full_page=False)
 assert page.locator('nav a').count()>=14
 page.locator('#dataset').select_option('sbe');page.locator('#filter').fill('256');assert '8 条' in page.locator('#count').inner_text()
 desktop_overflow=page.evaluate('document.documentElement.scrollWidth>innerWidth')
 page.set_viewport_size({'width':390,'height':844});page.set_content(p.read_text(), wait_until='load');page.wait_for_timeout(100)
 mobile_overflow=page.evaluate('document.documentElement.scrollWidth>innerWidth')
 page.screenshot(path=str(base/'report-mobile-preview.png'),full_page=False)
 assert not errors, errors
 assert not desktop_overflow and not mobile_overflow
 out={'figures_loaded':4,'javascript_errors':errors,'desktop_horizontal_overflow':desktop_overflow,'mobile_horizontal_overflow':mobile_overflow,'interactive_filter':'SBE/256 -> 8 rows PASS'}
 (report_dir/'report_validation.json').write_text(json.dumps(out,indent=2))
 print(json.dumps(out))
 b.close()
