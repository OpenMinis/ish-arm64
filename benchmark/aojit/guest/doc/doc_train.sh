# recording workload for document generation (other sizes/seeds than the measured runs)
cd /tmp/aojit/doc && python3 pdf_task.py 60 4 1 /tmp/aojit/doc/train.pdf && python3 ppt_task.py 8 6 1 /tmp/aojit/doc/train.pptx && python3 pdf_task.py 25 2 3 /tmp/aojit/doc/train2.pdf && python3 ppt_task.py 3 12 3 /tmp/aojit/doc/train2.pptx
