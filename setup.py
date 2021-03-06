#!/usr/bin/env python

from setuptools import setup

setup(name             = 'aduana',
      version          = '0.2.2',
      description      = 'Bindings for Aduana library',
      url              = 'https://github.com/scrapinghub/aduana',
      author           = 'Aduana developers',
      maintainer       = 'Pedro Lopez-Adeva Fernandez-Layos',
      maintainer_email = 'pedro@scrapinghub.com',
      packages         = ['aduana'],
      scripts          = [
          'aduana/bin/aduana-server.py', 
          'aduana/bin/aduana-server-cert.py'
      ],
      setup_requires   = ['cffi'],
      install_requires = [
          'gevent',
          'falcon',
          'talons',
          'requests',
          'pyOpenSSL',
          'cffi'
      ],
      cffi_modules     = ['build_wrapper.py:ffi'],
      keywords         = ['crawler', 'frontier', 'scrapy', 'web', 'requests'],
      classifiers      = [
        #'Framework :: Crawl Frontier',
        'Development Status :: 4 - Beta',
        'Environment :: Console',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: BSD License',
        'Operating System :: POSIX',
        'Programming Language :: Python',
        'Programming Language :: Python :: 2',
        'Programming Language :: Python :: 2.7',
        'Topic :: Internet :: WWW/HTTP',
        'Topic :: Software Development :: Libraries :: Application Frameworks',
        'Topic :: Software Development :: Libraries :: Python Modules',
    ],
)
