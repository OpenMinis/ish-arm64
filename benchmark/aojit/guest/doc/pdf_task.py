"""PDF report generation like an agent's: reportlab document (paragraphs, styled tables, an embedded
matplotlib chart) plus a matplotlib PdfPages report. usage: pdf_task.py <rows> <pages> <seed> <out>"""
import io, random, sys
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import Image, PageBreak, Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle
rows, pages, seed, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
random.seed(seed)
words = 'agent deploy build error python node server token cache memory report invoice release test merge'.split()
styles = getSampleStyleSheet()
story = [Paragraph('Weekly agent report', styles['Title'])]
fig, ax = plt.subplots(figsize=(6, 3))
ax.plot([random.random() for _ in range(200)]); ax.set_title('load')
buf = io.BytesIO(); fig.savefig(buf, format='png', dpi=100); plt.close(fig); buf.seek(0)
for p in range(pages):
    story.append(Paragraph(f'Section {p + 1}', styles['Heading2']))
    for _ in range(6):
        story.append(Paragraph(' '.join(random.choice(words) for _ in range(60)), styles['BodyText']))
    data = [['id', 'name', 'status', 'value', 'owner']] + [[str(i), random.choice(words), random.choice(['ok', 'fail', 'wait']),
            f'{random.uniform(0, 1000):.2f}', random.choice(words)] for i in range(rows)]
    t = Table(data, repeatRows=1)
    t.setStyle(TableStyle([('BACKGROUND', (0, 0), (-1, 0), colors.lightblue), ('GRID', (0, 0), (-1, -1), 0.25, colors.grey),
                           ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.whitesmoke])]))
    story += [t, Spacer(1, 0.5 * cm)]
    if p % 2 == 0:
        buf.seek(0); story.append(Image(buf, width=15 * cm, height=7.5 * cm))
    story.append(PageBreak())
SimpleDocTemplate(out, pagesize=A4).build(story)
with PdfPages(out.replace('.pdf', '_charts.pdf')) as pdf:
    for k in range(4):
        fig, axs = plt.subplots(2, 2, figsize=(8, 6))
        for a in axs.flat:
            a.bar(range(12), [random.randint(1, 100) for _ in range(12)]); a.set_title(random.choice(words))
        pdf.savefig(fig); plt.close(fig)
print('pdf ok')
